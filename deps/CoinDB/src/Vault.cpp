///////////////////////////////////////////////////////////////////////////////
//
// Vault.cpp
//
// Copyright (c) 2013-2014 Eric Lombrozo
//
// All Rights Reserved.
//

#include "Vault.h"

#include <CoinQ_script.h>
#include <CoinQ_blocks.h>

#include "Database.hxx"

#include "../odb/Schema-odb.hxx"

#include <odb/transaction.hxx>
#include <odb/session.hxx>

#include <hash.h>
#include <MerkleTree.h>
#include <secp256k1.h>
#include <BigInt.h>

#include <sstream>
#include <fstream>
#include <algorithm>

#include <logger.h>

// support for boost serialization
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>

const std::size_t MAXSQLCLAUSES = 500;

using namespace CoinDB;

/*
 * class Vault implementation
*/
Vault::Vault(int argc, char** argv, bool create, uint32_t version)
    : db_(open_database(argc, argv, create))
{
    LOGGER(trace) << "Opened Vault" << std::endl;
//    if (create) setVersion(version);
}

#if defined(DATABASE_SQLITE)
Vault::Vault(const std::string& filename, bool create, uint32_t version)
    : db_(openDatabase(filename, create))
{
    LOGGER(trace) << "Opened Vault - filename: " << filename << " create: " << (create ? "true" : "false") << " version: " << version << std::endl;
//    if (create) setVersion(version);
}
#endif

///////////////////////
// GLOBAL OPERATIONS //
///////////////////////
uint32_t Vault::getHorizonTimestamp() const
{
    LOGGER(trace) << "Vault::getHorizonTimestamp()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getHorizonTimestamp_unwrapped();
}

uint32_t Vault::getHorizonTimestamp_unwrapped() const
{
    odb::result<HorizonTimestampView> r(db_->query<HorizonTimestampView>());
    if (r.empty()) return 0xffffffff;
    return r.begin()->timestamp;
}

std::vector<bytes_t> Vault::getLocatorHashes() const
{
    LOGGER(trace) << "Vault::getLocatorHashes()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getLocatorHashes_unwrapped();
}

std::vector<bytes_t> Vault::getLocatorHashes_unwrapped() const
{
    std::vector<bytes_t>  hashes;
    std::vector<uint32_t> heights;

    uint32_t i = getBestHeight_unwrapped();
    if (i == 0) return hashes;

    uint32_t n = 1;
    uint32_t step = 1;
    heights.push_back(i);
    while (step <= i)
    {
        i -= step;
        n++;
        if (n > 10) step *= 2;
        heights.push_back(i);
    }

    typedef odb::query<BlockHeader> query_t;
    odb::result<BlockHeader> r(db_->query<BlockHeader>(query_t::height.in_range(heights.begin(), heights.end()) + "ORDER BY" + query_t::height + "DESC"));
    for (auto& header: r) { hashes.push_back(header.hash()); }
    return hashes;
}

Coin::BloomFilter Vault::getBloomFilter(double falsePositiveRate, uint32_t nTweak, uint32_t nFlags) const
{
    LOGGER(trace) << "Vault::getBloomFilter(" << falsePositiveRate << ", " << nTweak << ", " << nFlags << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getBloomFilter_unwrapped(falsePositiveRate, nTweak, nFlags);
}

Coin::BloomFilter Vault::getBloomFilter_unwrapped(double falsePositiveRate, uint32_t nTweak, uint32_t nFlags) const
{
    using namespace CoinQ::Script;

    std::vector<bytes_t> elements;
    odb::result<SigningScriptView> r(db_->query<SigningScriptView>());
    for (auto& view: r)
    {
        Script script(view.txinscript);
        elements.push_back(script.txinscript(Script::SIGN));                // Add input script element
        elements.push_back(getScriptPubKeyPayee(view.txoutscript).second);  // Add output script element
    }
    if (elements.empty()) return Coin::BloomFilter();

    Coin::BloomFilter filter(elements.size(), falsePositiveRate, nTweak, nFlags);
    for (auto& element: elements) { filter.insert(element); }
    return filter;
}

/////////////////////
// FILE OPERATIONS //
/////////////////////
void Vault::exportKeychain(const std::string& keychain_name, const std::string& filepath, bool exportprivkeys) const
{
    LOGGER(trace) << "Vault::exportKeychain(" << keychain_name << ", " << filepath << ", " << (exportprivkeys ? "true" : "false") << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Keychain> keychain = getKeychain_unwrapped(keychain_name);
    if (exportprivkeys && !keychain->isPrivate()) throw KeychainIsNotPrivateException(keychain_name);
    if (!exportprivkeys) { keychain->clearPrivateKey(); }
    exportKeychain_unwrapped(keychain, filepath);
}

void Vault::exportKeychain_unwrapped(std::shared_ptr<Keychain> keychain, const std::string& filepath) const
{
    std::ofstream ofs(filepath);
    boost::archive::text_oarchive oa(ofs);
    oa << *keychain;
}

std::shared_ptr<Keychain> Vault::importKeychain(const std::string& filepath, bool& importprivkeys)
{
    LOGGER(trace) << "Vault::importKeychain(" << filepath << ", " << (importprivkeys ? "true" : "false") << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Keychain> keychain = importKeychain_unwrapped(filepath, importprivkeys);
    t.commit();
    return keychain;
}

std::shared_ptr<Keychain> Vault::importKeychain_unwrapped(const std::string& filepath, bool& importprivkeys)
{
    std::shared_ptr<Keychain> keychain(new Keychain());
    {
        std::ifstream ifs(filepath);
        boost::archive::text_iarchive ia(ifs);
        ia >> *keychain;
    }

    if (!keychain->isPrivate()) { importprivkeys = false; }
    if (!importprivkeys)        { keychain->clearPrivateKey(); }

    odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::hash == keychain->hash()));
    if (!r.empty())
    {
        std::shared_ptr<Keychain> stored_keychain(r.begin().load());
        if (keychain->isPrivate() && !stored_keychain->isPrivate())
        {
            // Just import the private keys
            stored_keychain->importPrivateKey(*keychain);
            db_->update(stored_keychain);
            return stored_keychain; 
        }
        throw KeychainAlreadyExistsException(stored_keychain->name());
    }

    std::string keychain_name = keychain->name();
    unsigned int append_num = 1; // in case of name conflict
    while (keychainExists_unwrapped(keychain->name()))
    {
        std::stringstream ss;
        ss << keychain_name << append_num++;
        keychain->name(ss.str());
    }

    db_->persist(keychain);
    return keychain;
}

void Vault::exportAccount(const std::string& account_name, const std::string& filepath, const secure_bytes_t& chain_code_lock_key, const bytes_t& salt, bool exportprivkeys) const
{
    LOGGER(trace) << "Vault::exportAccount(" << account_name << ", " << filepath << ", " << (exportprivkeys ? "true" : "false") << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Account> account = getAccount_unwrapped(account_name);

    // Use the same lock key for all keychain chain codes
    tryUnlockAccountChainCodes_unwrapped(account);
    trySetAccountChainCodesLockKey_unwrapped(account, chain_code_lock_key, salt);

    if (!exportprivkeys)
        for (auto& keychain: account->keychains()) { keychain->clearPrivateKey(); }

    exportAccount_unwrapped(account, filepath);
}

void Vault::exportAccount_unwrapped(const std::shared_ptr<Account> account, const std::string& filepath) const
{

    std::ofstream ofs(filepath);
    boost::archive::text_oarchive oa(ofs);
    oa << *account;
}

std::shared_ptr<Account> Vault::importAccount(const std::string& filepath, const secure_bytes_t& chain_code_key, unsigned int& privkeysimported)
{
    LOGGER(trace) << "Vault::importAccount(" << filepath << ", " << privkeysimported << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Account> account = importAccount_unwrapped(filepath, chain_code_key, privkeysimported);
    t.commit();
    return account; 
}

std::shared_ptr<Account> Vault::importAccount_unwrapped(const std::string& filepath, const secure_bytes_t& chain_code_key, unsigned int& privkeysimported)
{
    std::shared_ptr<Account> account(new Account());
    {
        std::ifstream ifs(filepath);
        boost::archive::text_iarchive ia(ifs);
        ia >> *account;
    }

    odb::result<Account> r(db_->query<Account>(odb::query<Account>::hash == account->hash()));
    if (!r.empty()) throw AccountAlreadyExistsException(r.begin().load()->name());

    // In case of account name conflict
    std::string account_name = account->name();
    unsigned int append_num = 1;
    while (accountExists_unwrapped(account->name()))
    {
        std::stringstream ss;
        ss << account_name << append_num++;
        account->name(ss.str());
    }

    // Persist keychains
    bool countprivkeys = (privkeysimported != 0);
    privkeysimported = 0;
    KeychainSet keychains = account->keychains(); // We will replace any duplicate loaded keychains with keychains already in database.
    for (auto& keychain: account->keychains())
    {
        // Try to unlock account chain code
        if (!keychain->unlockChainCode(chain_code_key)) throw KeychainChainCodeUnlockFailedException(keychain->name());

        if (countprivkeys) { if (keychain->isPrivate()) privkeysimported++; }
        else               { keychain->clearPrivateKey(); }

        // If we already have the keychain, just import the private key if necessary
        odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::hash == keychain->hash()));
        if (!r.empty())
        {
            // TODO: This might be dangerous - we could end up overwriting a good keychain with a corrupt one. More checks necessary.
            // Perhaps we just disallow importing keychain before an account using it. Instead, first import account, then upgrade keychain to private.
            std::shared_ptr<Keychain> stored_keychain(r.begin().load());
            tryUnlockKeychainChainCode_unwrapped(stored_keychain);
            stored_keychain->setChainCodeLockKey(chain_code_key); // TODO: we should really use a single chain code key for the whole vault.
            if (keychain->isPrivate() && !stored_keychain->isPrivate())
                stored_keychain->importPrivateKey(*keychain);
            keychains.erase(keychain);
            keychains.insert(stored_keychain);
            db_->update(stored_keychain);
            continue;
        }

        std::string keychain_name = keychain->name();
        unsigned int append_num = 1; // in case of name conflict
        while (keychainExists_unwrapped(keychain->name()))
        {
            std::stringstream ss;
            ss << keychain_name << append_num++;
            keychain->name(ss.str());
        }

        db_->persist(keychain);
    }

    account->keychains(keychains); // We might have replaced loaded keychains with stored keychains.
    db_->persist(account);

    // Create signing scripts and keys and persist account bins
    for (auto& bin: account->bins())
    {
        db_->persist(bin);

        SigningScript::status_t status = bin->isChange() ? SigningScript::CHANGE : SigningScript::ISSUED;
        unsigned int next_script_index = bin->next_script_index();
        for (unsigned int i = 0; i < next_script_index; i++)
        {
            // TODO: SigningScript labels
            std::shared_ptr<SigningScript> script = bin->newSigningScript();
            script->status(status);
            for (auto& key: script->keys()) { db_->persist(key); }
            db_->persist(script); 
        }
        for (unsigned int i = 0; i < account->unused_pool_size(); i++)
        {
            std::shared_ptr<SigningScript> script = bin->newSigningScript();
            for (auto& key: script->keys()) { db_->persist(key); }
            db_->persist(script); 
        }
        db_->update(bin);
    } 

    // Persist account
    db_->update(account);
    return account;
}


/////////////////////////
// KEYCHAIN OPERATIONS //
/////////////////////////
bool Vault::keychainExists(const std::string& keychain_name) const
{
    LOGGER(trace) << "Vault::keychainExists(" << keychain_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return keychainExists_unwrapped(keychain_name);
}

bool Vault::keychainExists_unwrapped(const std::string& keychain_name) const
{
    odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::name == keychain_name));
    return !r.empty();
}

bool Vault::keychainExists(const bytes_t& keychain_hash) const
{
    LOGGER(trace) << "Vault::keychainExists(@hash = " << uchar_vector(keychain_hash).getHex() << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return keychainExists_unwrapped(keychain_hash);
}

bool Vault::keychainExists_unwrapped(const bytes_t& keychain_hash) const
{
    odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::hash == keychain_hash));
    return !r.empty();
}

std::shared_ptr<Keychain> Vault::newKeychain(const std::string& keychain_name, const secure_bytes_t& entropy, const secure_bytes_t& lockKey, const bytes_t& salt)
{
    LOGGER(trace) << "Vault::newKeychain(" << keychain_name << ", ...)" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session session;
    odb::core::transaction t(db_->begin());
    odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::name == keychain_name));
    if (!r.empty()) throw KeychainAlreadyExistsException(keychain_name);

    std::shared_ptr<Keychain> keychain(new Keychain(keychain_name, entropy, lockKey, salt));
    persistKeychain_unwrapped(keychain);
    t.commit();

    return keychain;
}

void Vault::renameKeychain(const std::string& old_name, const std::string& new_name)
{
    LOGGER(trace) << "Vault::renameKeychain(" << old_name << ", " << new_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session session;
    odb::core::transaction t(db_->begin());

    odb::result<Keychain> keychain_r(db_->query<Keychain>(odb::query<Keychain>::name == old_name));
    if (keychain_r.empty()) throw KeychainNotFoundException(old_name);

    if (old_name == new_name) return;

    odb::result<Keychain> new_keychain_r(db_->query<Keychain>(odb::query<Keychain>::name == new_name));
    if (!new_keychain_r.empty()) throw KeychainAlreadyExistsException(new_name);

    std::shared_ptr<Keychain> keychain(keychain_r.begin().load());
    keychain->name(new_name);

    db_->update(keychain);
    t.commit();
}

void Vault::persistKeychain_unwrapped(std::shared_ptr<Keychain> keychain)
{
    if (keychain->parent())
        db_->update(keychain->parent());

    db_->persist(keychain);
}

void Vault::tryUnlockAccountChainCodes_unwrapped(std::shared_ptr<Account> account) const
{
    std::set<std::string> locked_keychains;
    for (auto& keychain: account->keychains())
    {
        const auto& it = mapChainCodeUnlock.find(keychain->name());
        if (it == mapChainCodeUnlock.end())
        {
            locked_keychains.insert(keychain->name());
        }
        else
        {
            if (!keychain->unlockChainCode(it->second))
            {
                mapChainCodeUnlock.erase(keychain->name());
                locked_keychains.insert(keychain->name());
            }
        }
    }
    if (!locked_keychains.empty()) throw AccountChainCodeLockedException(account->name(), locked_keychains);
}

void Vault::trySetAccountChainCodesLockKey_unwrapped(std::shared_ptr<Account> account, const secure_bytes_t& new_lock_key, const bytes_t& salt) const
{
    for (auto& keychain: account->keychains()) { keychain->setChainCodeLockKey(new_lock_key, salt); }
}

std::shared_ptr<Keychain> Vault::getKeychain(const std::string& keychain_name) const
{
    LOGGER(trace) << "Vault::getKeychain(" << keychain_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getKeychain_unwrapped(keychain_name);
}

std::shared_ptr<Keychain> Vault::getKeychain_unwrapped(const std::string& keychain_name) const
{
    odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::name == keychain_name));
    if (r.empty()) throw KeychainNotFoundException(keychain_name);

    std::shared_ptr<Keychain> keychain(r.begin().load());
    return keychain;
}

std::vector<std::shared_ptr<Keychain>> Vault::getAllKeychains(bool root_only) const
{
    LOGGER(trace) << "Vault::getAllKeychains()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    odb::result<Keychain> r;
    if (root_only)
        r = db_->query<Keychain>(odb::query<Keychain>::parent.is_null());
    else
        r = db_->query<Keychain>();

    std::vector<std::shared_ptr<Keychain>> keychains;
    for (auto& keychain: r) { keychains.push_back(keychain.get_shared_ptr()); }
    return keychains;
}

void Vault::lockAllKeychainChainCodes()
{
    LOGGER(trace) << "Vault::lockAllKeychainChainCodes()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    mapChainCodeUnlock.clear();
}

void Vault::lockKeychainChainCode(const std::string& keychain_name)
{
    LOGGER(trace) << "Vault::lockKeychainChainCode(" << keychain_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    mapChainCodeUnlock.erase(keychain_name);
}

void Vault::unlockKeychainChainCode(const std::string& keychain_name, const secure_bytes_t& unlock_key)
{
    LOGGER(trace) << "Vault::unlockKeychainChainCode(" << keychain_name << ", ?)" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Keychain> keychain = getKeychain_unwrapped(keychain_name);
    if (keychain->unlockChainCode(unlock_key))
        mapChainCodeUnlock[keychain_name] = unlock_key;
}

bool Vault::tryUnlockKeychainChainCode_unwrapped(std::shared_ptr<Keychain> keychain) const
{
    const auto& it = mapChainCodeUnlock.find(keychain->name());
    if (it == mapChainCodeUnlock.end()) return false;
    if (!keychain->unlockChainCode(it->second))
        throw KeychainChainCodeUnlockFailedException(keychain->name());

    return true;
}
 
void Vault::lockAllKeychainPrivateKeys()
{
    LOGGER(trace) << "Vault::lockAllKeychainPrivateKeys()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    mapPrivateKeyUnlock.clear();
}

void Vault::lockKeychainPrivateKey(const std::string& keychain_name)
{
    LOGGER(trace) << "Vault::lockKeychainPrivateKey(" << keychain_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    mapPrivateKeyUnlock.erase(keychain_name);
}

void Vault::unlockKeychainPrivateKey(const std::string& keychain_name, const secure_bytes_t& unlock_key)
{
    LOGGER(trace) << "Vault::unlockKeychainPrivateKey(" << keychain_name << ", ?)" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Keychain> keychain = getKeychain_unwrapped(keychain_name);
    if (keychain->unlockPrivateKey(unlock_key))
        mapPrivateKeyUnlock[keychain_name] = unlock_key;
}

bool Vault::tryUnlockKeychainPrivateKey_unwrapped(std::shared_ptr<Keychain> keychain) const
{
    const auto& it = mapPrivateKeyUnlock.find(keychain->name());
    if (it == mapPrivateKeyUnlock.end()) return false;
    if (!keychain->unlockPrivateKey(it->second))
        throw KeychainPrivateKeyUnlockFailedException(keychain->name());

    return true;
}


////////////////////////
// ACCOUNT OPERATIONS //
////////////////////////    
bool Vault::accountExists(const std::string& account_name) const
{
    LOGGER(trace) << "Vault::accountExists(" << account_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return accountExists_unwrapped(account_name);
}

bool Vault::accountExists_unwrapped(const std::string& account_name) const
{
    odb::result<Account> r(db_->query<Account>(odb::query<Account>::name == account_name));
    return !r.empty();
}

void Vault::newAccount(const std::string& account_name, unsigned int minsigs, const std::vector<std::string>& keychain_names, uint32_t unused_pool_size, uint32_t time_created)
{
    LOGGER(trace) << "Vault::newAccount(" << account_name << ", " << minsigs << " of [" << stdutils::delimited_list(keychain_names, ", ") << "], " << unused_pool_size << ", " << time_created << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    odb::result<Account> r(db_->query<Account>(odb::query<Account>::name == account_name));
    if (!r.empty()) throw AccountAlreadyExistsException(account_name);

    KeychainSet keychains;
    for (auto& keychain_name: keychain_names)
    {
        odb::result<Keychain> r(db_->query<Keychain>(odb::query<Keychain>::name == keychain_name));
        if (r.empty()) throw KeychainNotFoundException(keychain_name);
        keychains.insert(r.begin().load());
    }

    std::shared_ptr<Account> account(new Account(account_name, minsigs, keychains, unused_pool_size, time_created));
    tryUnlockAccountChainCodes_unwrapped(account);
    db_->persist(account);

    // The first bin we create must be the change bin.
    std::shared_ptr<AccountBin> changeAccountBin= account->addBin(CHANGE_BIN_NAME);
    db_->persist(changeAccountBin);

    // The second bin we create must be the default bin.
    std::shared_ptr<AccountBin> defaultAccountBin = account->addBin(DEFAULT_BIN_NAME);
    db_->persist(defaultAccountBin);

    for (uint32_t i = 0; i < unused_pool_size; i++)
    {
        std::shared_ptr<SigningScript> changeSigningScript = changeAccountBin->newSigningScript();
        for (auto& key: changeSigningScript->keys()) { db_->persist(key); } 
        db_->persist(changeSigningScript);

        std::shared_ptr<SigningScript> defaultSigningScript = defaultAccountBin->newSigningScript();
        for (auto& key: defaultSigningScript->keys()) { db_->persist(key); }
        db_->persist(defaultSigningScript);
    }
    db_->update(changeAccountBin);
    db_->update(defaultAccountBin);
    db_->update(account);
    t.commit();
}

void Vault::renameAccount(const std::string& old_name, const std::string& new_name)
{
    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session session;
    odb::core::transaction t(db_->begin());

    odb::result<Account> account_r(db_->query<Account>(odb::query<Account>::name == old_name));
    if (account_r.empty()) throw std::runtime_error("Account not found.");

    if (old_name == new_name) return;

    odb::result<Account> new_account_r(db_->query<Account>(odb::query<Account>::name == new_name));
    if (!new_account_r.empty()) throw std::runtime_error("An account with that name already exists.");

    std::shared_ptr<Account> account(account_r.begin().load());
    account->name(new_name);

    db_->update(account);
    t.commit();
}

std::shared_ptr<Account> Vault::getAccount(const std::string& account_name) const
{
    LOGGER(trace) << "Vault::getAccount(" << account_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getAccount_unwrapped(account_name);
}

std::shared_ptr<Account> Vault::getAccount_unwrapped(const std::string& account_name) const
{
    odb::result<Account> r(db_->query<Account>(odb::query<Account>::name == account_name));
    if (r.empty()) throw AccountNotFoundException(account_name);

    std::shared_ptr<Account> account(r.begin().load());
    return account;
}

AccountInfo Vault::getAccountInfo(const std::string& account_name) const
{
    LOGGER(trace) << "Vault::getAccountInfo(" << account_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Account> account = getAccount_unwrapped(account_name);
    return account->accountInfo();
}

std::vector<AccountInfo> Vault::getAllAccountInfo() const
{
    LOGGER(trace) << "Vault::getAllAccountInfo()" << std::endl;
 
    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    odb::result<Account> r(db_->query<Account>());
    std::vector<AccountInfo> accountInfoVector;
    for (auto& account: r) { accountInfoVector.push_back(account.accountInfo()); }
    return accountInfoVector;
}

uint64_t Vault::getAccountBalance(const std::string& account_name, unsigned int min_confirmations, int tx_flags) const
{
    LOGGER(trace) << "Vault::getAccountBalance(" << account_name << ", " << min_confirmations << ")" << std::endl;

    std::vector<Tx::status_t> tx_statuses = Tx::getStatusFlags(tx_flags);

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    typedef odb::query<BalanceView> query_t;
    query_t query(query_t::Account::name == account_name && query_t::TxOut::status == TxOut::UNSPENT && query_t::Tx::status.in_range(tx_statuses.begin(), tx_statuses.end()));
    if (min_confirmations > 0)
    {
        odb::result<BestHeightView> height_r(db_->query<BestHeightView>());
        uint32_t best_height = height_r.empty() ? 0 : height_r.begin().load()->best_height;
        if (min_confirmations > best_height) return 0;
        query = (query && query_t::BlockHeader::height <= best_height + 1 - min_confirmations);
    }
    odb::result<BalanceView> r(db_->query<BalanceView>(query));
    return r.empty() ? 0 : r.begin().load()->balance;
}

std::shared_ptr<AccountBin> Vault::addAccountBin(const std::string& account_name, const std::string& bin_name)
{
    LOGGER(trace) << "Vault::addAccountBin(" << account_name << ", " << bin_name << ")" << std::endl;

    if (bin_name.empty() || bin_name[0] == '@') throw std::runtime_error("Invalid account bin name.");

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());

    bool binExists = true;
    try
    {
        getAccountBin_unwrapped(account_name, bin_name);
    }
    catch (const AccountBinNotFoundException& e)
    {
        binExists = false;
    }

    if (binExists) throw AccountBinAlreadyExistsException(account_name, bin_name);

    std::shared_ptr<Account> account = getAccount_unwrapped(account_name);
    tryUnlockAccountChainCodes_unwrapped(account);

    std::shared_ptr<AccountBin> bin = account->addBin(bin_name);
    db_->persist(bin);

    for (uint32_t i = 0; i < account->unused_pool_size(); i++)
    {
        std::shared_ptr<SigningScript> script = bin->newSigningScript();
        for (auto& key: script->keys()) { db_->persist(key); }
        db_->persist(script);
    }
    db_->update(bin);
    db_->update(account);
    t.commit();

    return bin;
}

std::shared_ptr<SigningScript> Vault::issueSigningScript(const std::string& account_name, const std::string& bin_name, const std::string& label)
{
    LOGGER(trace) << "Vault::issueSigningScript(" << account_name << ", " << bin_name << ", " << label << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<AccountBin> bin = getAccountBin_unwrapped(account_name, bin_name);
    std::shared_ptr<SigningScript> script = issueAccountBinSigningScript_unwrapped(bin, label);
    t.commit();
    return script;
}

std::shared_ptr<SigningScript> Vault::issueAccountBinSigningScript_unwrapped(std::shared_ptr<AccountBin> bin, const std::string& label)
{
    if (bin->isChange()) throw AccountCannotIssueChangeScriptException(bin->account()->name());

    try
    {
        refillAccountBinPool_unwrapped(bin);
    }
    catch (const AccountChainCodeLockedException& e)
    {
        LOGGER(debug) << "Vault::issueAccountBinSigningScript_unwrapped(" << bin->account()->name() << "::" << bin->name() << ", " << label << ") - Chain code is locked so pool cannot be replenished." << std::endl;
    }

    // Get the next available unused signing script
    typedef odb::query<SigningScriptView> view_query;
    odb::result<SigningScriptView> view_result(db_->query<SigningScriptView>(
        (view_query::AccountBin::id == bin->id() && view_query::SigningScript::status == SigningScript::UNUSED) +
        "ORDER BY" + view_query::SigningScript::index + "LIMIT 1"));
    if (view_result.empty()) throw AccountBinOutOfScriptsException(bin->account()->name(), bin->name());

    std::shared_ptr<SigningScriptView> view(view_result.begin().load());

    typedef odb::query<SigningScript> script_query;
    odb::result<SigningScript> script_result(db_->query<SigningScript>(script_query::id == view->id));
    std::shared_ptr<SigningScript> script(script_result.begin().load());
    script->label(label);
    script->status(SigningScript::ISSUED);
    db_->update(script);
    db_->update(script->account_bin());
    return script;
}

void Vault::refillAccountPool(const std::string& account_name)
{
    LOGGER(trace) << "Vault::refillAccountPool(" << account_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Account> account = getAccount_unwrapped(account_name);
    refillAccountPool_unwrapped(account);
    t.commit();
}

void Vault::refillAccountPool_unwrapped(std::shared_ptr<Account> account)
{
    for (auto& bin: account->bins()) { refillAccountBinPool_unwrapped(bin); }
}

void Vault::refillAccountBinPool_unwrapped(std::shared_ptr<AccountBin> bin)
{
    tryUnlockAccountChainCodes_unwrapped(bin->account());

    typedef odb::query<ScriptCountView> count_query;
    odb::result<ScriptCountView> count_result(db_->query<ScriptCountView>(count_query::AccountBin::id == bin->id() && count_query::SigningScript::status == SigningScript::UNUSED));
    uint32_t count = count_result.empty() ? 0 : count_result.begin().load()->count;

    uint32_t unused_pool_size = bin->account()->unused_pool_size();
    for (uint32_t i = count; i < unused_pool_size; i++)
    {
        std::shared_ptr<SigningScript> script = bin->newSigningScript();
        for (auto& key: script->keys()) { db_->persist(key); }
        db_->persist(script); 
    } 
    db_->update(bin);
}

std::vector<SigningScriptView> Vault::getSigningScriptViews(const std::string& account_name, const std::string& bin_name, int flags) const
{
    LOGGER(trace) << "Vault::getSigningScriptViews(" << account_name << ", " << bin_name << ", " << SigningScript::getStatusString(flags) << ")" << std::endl;

    std::vector<SigningScript::status_t> statusRange = SigningScript::getStatusFlags(flags);

    typedef odb::query<SigningScriptView> query_t;
    query_t query(query_t::SigningScript::status.in_range(statusRange.begin(), statusRange.end()));
    if (account_name != "@all") query = (query && query_t::Account::name == account_name);
    if (bin_name != "@all")     query = (query && query_t::AccountBin::name == bin_name);
    query += "ORDER BY" + query_t::Account::name + "ASC," + query_t::AccountBin::name + "ASC," + query_t::SigningScript::status + "DESC," + query_t::SigningScript::index + "ASC";

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());

    std::vector<SigningScriptView> views;
    odb::result<SigningScriptView> r(db_->query<SigningScriptView>(query));
    for (auto& view: r) { views.push_back(view); }
    return views;
}

std::vector<TxOutView> Vault::getTxOutViews(const std::string& account_name, const std::string& bin_name, int txout_status_flags, int tx_status_flags) const
{
    LOGGER(trace) << "Vault::getTxOutViews(" << account_name << ", " << bin_name << ", " << TxOut::getStatusString(txout_status_flags) << ", " << ", " << Tx::getStatusString(tx_status_flags) << ")" << std::endl;

    typedef odb::query<TxOutView> query_t;
    query_t query(query_t::receiving_account::id != 0 || query_t::sending_account::id != 0);
    if (account_name != "@all")                 query = (query && (query_t::sending_account::name == account_name || query_t::receiving_account::name == account_name));
    if (bin_name != "@all")                     query = (query && query_t::AccountBin::name == bin_name);

    std::vector<TxOut::status_t> txout_statuses = TxOut::getStatusFlags(txout_status_flags);
    query = (query && query_t::TxOut::status.in_range(txout_statuses.begin(), txout_statuses.end()));

    std::vector<Tx::status_t> tx_statuses = Tx::getStatusFlags(tx_status_flags);
    query = (query && query_t::Tx::status.in_range(tx_statuses.begin(), tx_statuses.end()));

    query += "ORDER BY" + query_t::BlockHeader::height + "DESC," + query_t::Tx::timestamp + "DESC," + query_t::Tx::id + "DESC";

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    std::vector<TxOutView> views;
    odb::result<TxOutView> r(db_->query<TxOutView>(query));
    for (auto& view: r) { views.push_back(view); }
    return views;
}


////////////////////////////
// ACCOUNT BIN OPERATIONS //
////////////////////////////    
std::shared_ptr<AccountBin> Vault::getAccountBin(const std::string& account_name, const std::string& bin_name) const
{
    LOGGER(trace) << "Vault::getAccountBin(" << account_name << ", " << bin_name << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<AccountBin> bin = getAccountBin_unwrapped(account_name, bin_name);
    return bin; 
}

std::shared_ptr<AccountBin> Vault::getAccountBin_unwrapped(const std::string& account_name, const std::string& bin_name) const
{
    typedef odb::query<AccountBinView> query;
    odb::result<AccountBinView> r(db_->query<AccountBinView>(query::Account::name == account_name && query::AccountBin::name == bin_name));
    if (r.empty()) throw AccountBinNotFoundException(account_name, bin_name);

    unsigned long bin_id = r.begin().load()->bin_id;
    std::shared_ptr<AccountBin> bin(db_->load<AccountBin>(bin_id));
    return bin;
}


////////////////////////////
// TRANSACTION OPERATIONS //
////////////////////////////
std::shared_ptr<Tx> Vault::getTx(const bytes_t& hash) const
{
    LOGGER(trace) << "Vault::getTx(" << uchar_vector(hash).getHex() << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    return getTx_unwrapped(hash);
}

std::shared_ptr<Tx> Vault::getTx_unwrapped(const bytes_t& hash) const
{
    odb::result<Tx> r(db_->query<Tx>(odb::query<Tx>::hash == hash || odb::query<Tx>::unsigned_hash == hash));
    if (r.empty()) throw TxNotFoundException(hash);

    std::shared_ptr<Tx> tx(r.begin().load());
    return tx;
}

std::shared_ptr<Tx> Vault::insertTx(std::shared_ptr<Tx> tx)
{
    LOGGER(trace) << "Vault::insertTx(...) - hash: " << uchar_vector(tx->hash()).getHex() << ", unsigned hash: " << uchar_vector(tx->unsigned_hash()).getHex() << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    tx = insertTx_unwrapped(tx);
    if (tx) t.commit();
    return tx;
}

std::shared_ptr<Tx> Vault::insertTx_unwrapped(std::shared_ptr<Tx> tx)
{
    // TODO: Validate signatures
    tx->updateStatus();

    odb::result<Tx> tx_r(db_->query<Tx>(odb::query<Tx>::unsigned_hash == tx->unsigned_hash()));

    // First handle situations where we have a duplicate
    if (!tx_r.empty())
    {
        LOGGER(debug) << "Vault::insertTx_unwrapped - We have a transaction with the same unsigned hash: " << uchar_vector(tx->unsigned_hash()).getHex() << std::endl;
        std::shared_ptr<Tx> stored_tx(tx_r.begin().load());

        // First handle situations where the transaction we currently have is not fully signed.
        if (stored_tx->status() == Tx::UNSIGNED)
        {
            if (tx->status() != Tx::UNSIGNED)
            {
                // The transaction we received is a signed version of the one we had unsigned, so replace
                LOGGER(debug) << "Vault::insertTx_unwrapped - REPLACING OLD UNSIGNED TRANSACTION WITH NEW SIGNED TRANSACTION. hash: " << uchar_vector(tx->hash()).getHex() << std::endl; 
                std::size_t i = 0;
                txins_t txins = tx->txins();
                for (auto& txin: stored_tx->txins())
                {
                    txin->script(txins[i++]->script());
                    db_->update(txin);
                }
                stored_tx->updateStatus(tx->status());
                db_->update(stored_tx);
                return stored_tx;
            }
            else
            {
                // The transaction we received is unsigned but might have more signatures. Only add new signatures.
                bool updated = false;
                std::size_t i = 0;
                txins_t txins = tx->txins();
                for (auto& txin: stored_tx->txins())
                {
                    using namespace CoinQ::Script;
                    Script stored_script(txin->script());
                    Script new_script(txins[i]->script());
                    unsigned int sigsadded = stored_script.mergesigs(new_script);
                    if (sigsadded > 0)
                    {
                        LOGGER(debug) << "Vault::insertTx_unwrapped - ADDED " << sigsadded << " NEW SIGNATURE(S) TO INPUT " << i << std::endl;
                        txin->script(stored_script.txinscript(Script::EDIT));
                        db_->update(txin);
                        updated = true;
                    }
                    i++;
                }
                return updated ? stored_tx : nullptr;
            }
        }
        else
        {
            // The transaction we currently have is already fully signed, so only update status if necessary
            if (tx->status() != Tx::UNSIGNED)
            {
                if (tx->status() > stored_tx->status())
                {
                    LOGGER(debug) << "Vault::insertTx_unwrapped - UPDATING TRANSACTION STATUS FROM " << stored_tx->status() << " TO " << tx->status() << ". hash: " << uchar_vector(stored_tx->hash()).getHex() << std::endl;
                    stored_tx->updateStatus(tx->status());
                    db_->update(stored_tx);
                    return stored_tx;
                }
                else
                {
                    LOGGER(debug) << "Vault::insertTx_unwrapped - Transaction not updated. hash: " << uchar_vector(stored_tx->hash()).getHex() << std::endl;
                    return nullptr;
                }
            }
            else
            {
                LOGGER(debug) << "Vault::insertTx_unwrapped - Stored transaction is already signed, received transaction is missing signatures. Ignore. hash: " << uchar_vector(stored_tx->hash()).getHex() << std::endl;
                return nullptr;
            }
        }
    }

    // If we get here it means we've either never seen this transaction before or it doesn't affect our accounts.

    std::set<std::shared_ptr<Tx>> conflicting_txs;
    std::set<std::shared_ptr<TxOut>> updated_txouts;

    // Check inputs
    bool sent_from_vault = false; // whether any of the inputs belong to vault
    bool have_all_outpoints = true; // whether we have all outpoints (for fee calculation)
    uint64_t input_total = 0;
    std::shared_ptr<Account> sending_account;

    for (auto& txin: tx->txins())
    {
        // Check if inputs connect
        tx_r = db_->query<Tx>(odb::query<Tx>::hash == txin->outhash());
        if (tx_r.empty())
        {
            // TODO: If the txinscript is in one of our accounts but we don't have the outpoint it means this transaction is orphaned.
            //       We should have an orphaned flag for the transaction. Otherwise out-of-order insertions will result in inconsistent state.
            have_all_outpoints = false;
        }
        else
        {
            std::shared_ptr<Tx> spent_tx(tx_r.begin().load());
            txouts_t outpoints = spent_tx->txouts();
            uint32_t outindex = txin->outindex();
            if (outpoints.size() <= outindex) throw std::runtime_error("Vault::insertTx_unwrapped - outpoint out of range.");
            std::shared_ptr<TxOut>& outpoint = outpoints[outindex];

            // Check for double spend, track conflicted transaction so we can update status if necessary later.
            std::shared_ptr<TxIn> conflict_txin = outpoint->spent();
            if (conflict_txin)
            {
                LOGGER(debug) << "Vault::insertTx_unwrapped - Discovered conflicting transaction. Double spend. hash: " << uchar_vector(conflict_txin->tx()->hash()).getHex() << std::endl;
                conflicting_txs.insert(conflict_txin->tx());
            } 

            input_total += outpoint->value();

            // Was this transaction signed using one of our accounts?
            odb::result<SigningScript> script_r(db_->query<SigningScript>(odb::query<SigningScript>::txoutscript == outpoint->script()));
            if (!script_r.empty())
            {
                sent_from_vault = true;
                outpoint->spent(txin);
                updated_txouts.insert(outpoint);
                if (!sending_account)
                {
                    // Assuming all inputs belong to the same account
                    // TODO: Allow coin mixing
                    std::shared_ptr<SigningScript> script(script_r.begin().load());
                    sending_account = script->account();
                }
            }
        }
    }

    // Stored for later update
    std::set<std::shared_ptr<SigningScript>> scripts;
    std::set<std::shared_ptr<AccountBin>> account_bins;

    // Check outputs
    bool sent_to_vault = false; // whether any of the outputs are spendable by accounts in vault
    uint64_t output_total = 0;

    for (auto& txout: tx->txouts())
    {
        output_total += txout->value();
        odb::result<SigningScript> script_r(db_->query<SigningScript>(odb::query<SigningScript>::txoutscript == txout->script()));
        if (!script_r.empty())
        {
            // This output is spendable from an account in the vault
            sent_to_vault = true;
            std::shared_ptr<SigningScript> script(script_r.begin().load());
            txout->signingscript(script);

            // Update the signing script and txout status
            switch (script->status())
            {
            case SigningScript::UNUSED:
                if (sent_from_vault && script->account_bin()->isChange())
                {
                    script->status(SigningScript::CHANGE);
                }
                else
                {
                    script->status(SigningScript::USED);
                }
                scripts.insert(script);
                account_bins.insert(script->account_bin());
                try
                {
                    refillAccountBinPool_unwrapped(script->account_bin());
                }
                catch (const AccountChainCodeLockedException& e)
                {
                    LOGGER(debug) << "Vault::insertTx_unwrapped - Chain code is locked so change pool cannot be replenished." << std::endl;
                }
                break;

            case SigningScript::ISSUED:
                script->status(SigningScript::USED);
                scripts.insert(script);
                break;

            default:
                break;
            }

            // Check if the output has already been spent (transactions inserted out of order)
            odb::result<TxIn> txin_r(db_->query<TxIn>(odb::query<TxIn>::outhash == tx->hash() && odb::query<TxIn>::outindex == txout->txindex()));
            if (!txin_r.empty())
            {
                std::shared_ptr<TxIn> txin(txin_r.begin().load());
                txout->spent(txin);
            }
        }
        else if (sending_account)
        {
            // Again, assume all inputs sent from same account.
            // TODO: Allow coin mixing.
            txout->sending_account(sending_account);
        }
    }

    if (!conflicting_txs.empty())
    {
        tx->updateStatus(Tx::CONFLICTING);
        for (auto& conflicting_tx: conflicting_txs)
        {
            if (conflicting_tx->status() != Tx::CONFIRMED)
            {
                conflicting_tx->updateStatus(Tx::CONFLICTING);
                db_->update(conflicting_tx);
            }
        }
    }

    if (sent_from_vault || sent_to_vault)
    {
        LOGGER(debug) << "Vault::insertTx_unwrapped - INSERTING NEW TRANSACTION. hash: " << uchar_vector(tx->hash()).getHex() << ", unsigned hash: " << uchar_vector(tx->unsigned_hash()).getHex() << std::endl;
        if (have_all_outpoints) { tx->fee(input_total - output_total); }

        // Persist the transaction
        db_->persist(*tx);
        for (auto& txin:        tx->txins())    { db_->persist(txin);       }
        for (auto& txout:       tx->txouts())   { db_->persist(txout);      }

        // Update other affected objects
        for (auto& script:      scripts)        { db_->update(script);      }
        for (auto& account_bin: account_bins)   { db_->update(account_bin); }
        for (auto& txout:       updated_txouts) { db_->update(txout);       }

        if (tx->status() >= Tx::SENT) updateConfirmations_unwrapped(tx);
        return tx;
    }

    LOGGER(debug) << "Vault::insertTx_unwrapped - transaction not inserted." << std::endl;
    return nullptr; 
}

std::shared_ptr<Tx> Vault::createTx(const std::string& account_name, uint32_t tx_version, uint32_t tx_locktime, txouts_t txouts, uint64_t fee, unsigned int maxchangeouts, bool insert)
{
    LOGGER(trace) << "Vault::createTx(" << account_name << ", " << tx_version << ", " << tx_locktime << ", " << txouts.size() << " txout(s), " << fee << ", " << maxchangeouts << ", " << (insert ? "insert" : "no insert") << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    std::shared_ptr<Tx> tx = createTx_unwrapped(account_name, tx_version, tx_locktime, txouts, fee, maxchangeouts);
    if (insert)
    {
        tx = insertTx_unwrapped(tx);
        if (tx) t.commit();
    } 
    return tx;
}

std::shared_ptr<Tx> Vault::createTx_unwrapped(const std::string& account_name, uint32_t tx_version, uint32_t tx_locktime, txouts_t txouts, uint64_t fee, unsigned int /*maxchangeouts*/)
{
    // TODO: Better rng seeding
    std::srand(std::time(0));

    // TODO: Better fee calculation heuristics
    uint64_t desired_total = fee;
    for (auto& txout: txouts) { desired_total += txout->value(); }

    std::shared_ptr<Account> account = getAccount_unwrapped(account_name);

    // TODO: Better coin selection
    typedef odb::query<TxOutView> query_t;
    odb::result<TxOutView> utxoview_r(db_->query<TxOutView>(query_t::TxOut::status == TxOut::UNSPENT && query_t::receiving_account::id == account->id()));
    std::vector<TxOutView> utxoviews;
    for (auto& utxoview: utxoview_r) { utxoviews.push_back(utxoview); }
   
    std::random_shuffle(utxoviews.begin(), utxoviews.end(), [](int i) { return std::rand() % i; });

    txins_t txins;
    int i = 0;
    uint64_t total = 0;
    for (auto& utxoview: utxoviews)
    {
        total += utxoview.value;
        std::shared_ptr<TxIn> txin(new TxIn(utxoview.tx_hash, utxoview.tx_index, utxoview.signingscript_txinscript, 0xffffffff));
        txins.push_back(txin);
        i++;
        if (total >= desired_total) break;
    }
    if (total < desired_total) throw AccountInsufficientFundsException(account_name); 

    utxoviews.resize(i);
    uint64_t change = total - desired_total;

    if (change > 0)
    {
        std::shared_ptr<AccountBin> bin = getAccountBin_unwrapped(account_name, CHANGE_BIN_NAME);
        std::shared_ptr<SigningScript> changescript = issueAccountBinSigningScript_unwrapped(bin);

        // TODO: Allow adding multiple change outputs
        std::shared_ptr<TxOut> txout(new TxOut(change, changescript));
        txouts.push_back(txout);
    }
    std::random_shuffle(txouts.begin(), txouts.end(), [](int i) { return std::rand() % i; });

    std::shared_ptr<Tx> tx(new Tx());
    tx->set(tx_version, txins, txouts, tx_locktime, time(NULL), Tx::UNSIGNED);
    return tx;
}

void Vault::updateTx_unwrapped(std::shared_ptr<Tx> tx)
{
    for (auto& txin: tx->txins()) { db_->update(txin); }
    for (auto& txout: tx->txouts()) { db_->update(txout); }
    db_->update(tx); 
}

void Vault::deleteTx(const bytes_t& tx_hash)
{
    LOGGER(trace) << "Vault::deleteTx(" << uchar_vector(tx_hash).getHex() << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    odb::result<Tx> r(db_->query<Tx>(odb::query<Tx>::hash == tx_hash || odb::query<Tx>::unsigned_hash == tx_hash));
    if (r.empty()) throw TxNotFoundException(tx_hash);

    std::shared_ptr<Tx> tx(r.begin().load());
    deleteTx_unwrapped(tx);
    t.commit();
}

void Vault::deleteTx_unwrapped(std::shared_ptr<Tx> tx)
{
    // NOTE: signingscript statuses are not updated. once received always received.

    // delete txins
    for (auto& txin: tx->txins())
    {
        // unspend spent outpoints first
        odb::result<TxOut> txout_r(db_->query<TxOut>(odb::query<TxOut>::spent == txin->id()));
        if (!txout_r.empty())
        {
            std::shared_ptr<TxOut> txout(txout_r.begin().load());
            txout->spent(nullptr);
            db_->update(txout);
        }
        db_->erase(txin);
    }

    // delete txouts
    for (auto& txout: tx->txouts())
    {
        // recursively delete any transactions that depend on this one first
        if (txout->spent()) { deleteTx_unwrapped(txout->spent()->tx()); }
        db_->erase(txout);
    }

    // delete tx
    db_->erase(tx);
}

SigningRequest Vault::getSigningRequest(const bytes_t& unsigned_hash, bool include_raw_tx) const
{
    LOGGER(trace) << "Vault::getSigningRequest(" << uchar_vector(unsigned_hash).getHex() << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    odb::result<Tx> r(db_->query<Tx>(odb::query<Tx>::unsigned_hash == unsigned_hash));
    if (r.empty()) throw TxNotFoundException(unsigned_hash);

    std::shared_ptr<Tx> tx(r.begin().load());
    return getSigningRequest_unwrapped(tx, include_raw_tx);
}

SigningRequest Vault::getSigningRequest_unwrapped(std::shared_ptr<Tx> tx, bool include_raw_tx) const
{
    unsigned int sigs_needed = tx->missingSigCount();
    std::set<bytes_t> pubkeys = tx->missingSigPubkeys();
    std::set<SigningRequest::keychain_info_t> keychain_info;
    odb::result<Key> key_r(db_->query<Key>(odb::query<Key>::pubkey.in_range(pubkeys.begin(), pubkeys.end())));
    for (auto& keychain: key_r)
    {
        std::shared_ptr<Keychain> root_keychain(keychain.root_keychain());
        keychain_info.insert(std::make_pair(root_keychain->name(), root_keychain->hash()));
    }

    bytes_t rawtx;
    if (include_raw_tx) rawtx = tx->raw();
    return SigningRequest(sigs_needed, keychain_info, rawtx);
}

bool Vault::signTx(const bytes_t& unsigned_hash, bool update)
{
    LOGGER(trace) << "Vault::signTx(" << uchar_vector(unsigned_hash).getHex() << ", " << (update ? "update" : "no update") << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());

    odb::result<Tx> tx_r(db_->query<Tx>(odb::query<Tx>::unsigned_hash == unsigned_hash));
    if (tx_r.empty()) throw TxNotFoundException(unsigned_hash);
    std::shared_ptr<Tx> tx(tx_r.begin().load());

    bool rval = signTx_unwrapped(tx);
    if (rval && update)
    {
        updateTx_unwrapped(tx);
        t.commit();
    }
    return rval;
}

bool Vault::signTx_unwrapped(std::shared_ptr<Tx> tx)
{
    using namespace CoinQ::Script;
    using namespace CoinCrypto;
    unsigned int sigsadded = 0;
    for (auto& txin: tx->txins())
    {
        Script script(txin->script());
        unsigned int sigsneeded = script.sigsneeded();
        if (sigsneeded == 0) continue;

        std::vector<bytes_t> pubkeys = script.missingsigs();
        if (pubkeys.empty()) continue;

        odb::result<Key> key_r(db_->query<Key>(odb::query<Key>::is_private != 0 && odb::query<Key>::pubkey.in_range(pubkeys.begin(), pubkeys.end())));
        if (key_r.empty()) continue;

        // Prepare the inputs for hashing
        Coin::Transaction coin_tx = tx->toCoinClasses();
        unsigned int i = 0;
        for (auto& coin_input: coin_tx.inputs)
        {
            if (i++ == txin->txindex()) { coin_input.scriptSig = script.txinscript(Script::SIGN); }
            else                        { coin_input.scriptSig.clear(); }
        }

        // Compute hash to sign
        bytes_t signingHash = coin_tx.getHashWithAppendedCode(SIGHASH_ALL);
        LOGGER(debug) << "Vault::signTx_unwrapped - computed signing hash " << uchar_vector(signingHash).getHex() << " for input " << txin->txindex() << std::endl;

        for (auto& key: key_r)
        {
            if (!tryUnlockKeychainPrivateKey_unwrapped(key.root_keychain()))
            {
                LOGGER(debug) << "Vault::signTx_unwrapped - private key locked for keychain " << key.root_keychain()->name() << std::endl;
                continue;
            }
            if (!tryUnlockKeychainChainCode_unwrapped(key.root_keychain()))
            {
                LOGGER(debug) << "Vault::signTx_unwrapped - chain code locked for keychain " << key.root_keychain()->name() << std::endl;
                continue;
            }

            LOGGER(debug) << "Vault::signTx_unwrapped - SIGNING INPUT " << txin->txindex() << " WITH KEYCHAIN " << key.root_keychain()->name() << std::endl;        
            secure_bytes_t privkey = key.try_privkey();

            // TODO: Better exception handling with secp256kl_key class
            secp256k1_key signingKey;
            signingKey.setPrivKey(privkey);
            if (signingKey.getPubKey() != key.pubkey()) throw KeychainInvalidPrivateKeyException(key.root_keychain()->name(), key.pubkey());

            bytes_t signature = secp256k1_sign(signingKey, signingHash);
            signature.push_back(SIGHASH_ALL);
            script.addSig(key.pubkey(), signature);
            LOGGER(debug) << "Vault::signTx_unwrapped - PUBLIC KEY: " << uchar_vector(key.pubkey()).getHex() << " SIGNATURE: " << uchar_vector(signature).getHex() << std::endl;
            sigsadded++;
            sigsneeded--;
            if (sigsneeded == 0) break;
        }

        txin->script(script.txinscript(sigsneeded ? Script::EDIT : Script::BROADCAST));
    }

    if (!sigsadded) return false;

    tx->updateStatus();
    return true;
}


///////////////////////////
// BLOCKCHAIN OPERATIONS //
///////////////////////////
uint32_t Vault::getBestHeight() const
{
    LOGGER(trace) << "Vault::getBestHeight()" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::transaction t(db_->begin());
    return getBestHeight_unwrapped();
}

uint32_t Vault::getBestHeight_unwrapped() const
{
    odb::result<BestHeightView> r(db_->query<BestHeightView>());
    uint32_t best_height = r.empty() ? 0 : r.begin()->best_height;
    return best_height;
}

bool Vault::insertMerkleBlock(std::shared_ptr<MerkleBlock> merkleblock)
{
    LOGGER(trace) << "Vault::insertMerkleBlock(" << uchar_vector(merkleblock->blockheader()->hash()).getHex() << ")" << std::endl;

    boost::lock_guard<boost::mutex> lock(mutex);
    odb::core::session s;
    odb::core::transaction t(db_->begin());
    return insertMerkleBlock_unwrapped(merkleblock);
}

bool Vault::insertMerkleBlock_unwrapped(std::shared_ptr<MerkleBlock> merkleblock)
{
    auto& new_header = merkleblock->blockheader();
    std::string hash_str = uchar_vector(new_header->hash()).getHex();

    // We need to start fetching no later than the block time horizon window
    odb::result<BlockHeader> block_r(db_->query<BlockHeader>(odb::query<BlockHeader>::hash == new_header->prevhash()));
    if (block_r.empty() && new_header->timestamp() + TIME_HORIZON_WINDOW > getHorizonTimestamp_unwrapped()) return false;

    block_r = db_->query<BlockHeader>(odb::query<BlockHeader>::hash == new_header->hash());
    if (!block_r.empty())
    {
        std::shared_ptr<BlockHeader> header(block_r.begin().load());
        LOGGER(debug) << "Vault::insertMerkleBlock_unwrapped - already have block. hash: " << hash_str << ", height: " << header->height() << std::endl;
        return false;
    }

    block_r = db_->query<BlockHeader>(odb::query<BlockHeader>::height >= new_header->height());
    if (!block_r.empty()) // Reorg
    {
        LOGGER(debug) << "Vault::insertMerkleBlock_unwrapped - reorganization. hash: " << hash_str << ", height: " << new_header->height() << std::endl;
        // Disconnect blocks
        for (auto& sidechain_header: block_r)
        {
            db_->erase_query<MerkleBlock>(odb::query<MerkleBlock>::blockheader == sidechain_header.id());
            odb::result<Tx> tx_r(db_->query<Tx>(odb::query<Tx>::blockheader == sidechain_header.id()));
            for (auto& tx: tx_r)
            {
                tx.blockheader(nullptr);
                db_->update(tx);
            }
        }
        db_->erase_query<BlockHeader>(odb::query<BlockHeader>::height >= new_header->height());
    }

    LOGGER(debug) << "Vault::insertMerkleBlock_unwrapped - inserting new merkle block. hash: " << hash_str << ", height: " << new_header->height() << std::endl;
    db_->persist(new_header);
    db_->persist(merkleblock);

    auto& hashes = merkleblock->hashes();
    odb::result<Tx> tx_r(db_->query<Tx>(odb::query<Tx>::hash.in_range(hashes.begin(), hashes.end())));
    for (auto& tx: tx_r)
    {
        LOGGER(debug) << "Vault::insertMerkleBlock_unwrapped - updating transaction. hash: " << uchar_vector(tx.hash()).getHex() << std::endl;
        tx.block(new_header, 0xffffffff); // TODO: compute correct index or eliminate index altogether/
        db_->update(tx);
    }

    unsigned int count = updateConfirmations_unwrapped();
    LOGGER(debug) << "Vault::insertMerkleBlock_unwrapped - " << count << " transaction(s) confirmed." << std::endl;
    return true;
}

unsigned int Vault::updateConfirmations_unwrapped(std::shared_ptr<Tx> tx)
{
    unsigned int count = 0;
    typedef odb::query<ConfirmedTxView> query_t;
    query_t query(query_t::Tx::blockheader.is_null());
    if (tx) query = (query && query_t::Tx::hash == tx->hash());

    odb::result<ConfirmedTxView> r(db_->query<ConfirmedTxView>(query));
    for (auto& view: r)
    {
        if (view.blockheader_id == 0) continue;

        std::shared_ptr<Tx> tx(db_->load<Tx>(view.tx_id));
        std::shared_ptr<BlockHeader> blockheader(db_->load<BlockHeader>(view.blockheader_id));
        tx->blockheader(blockheader);
        db_->update(tx);
        count++;
        LOGGER(debug) << "Vault::updateConfirmations_unwrapped - transaction " << uchar_vector(tx->hash()).getHex() << " confirmed in block " << uchar_vector(tx->blockheader()->hash()).getHex() << " height: " << tx->blockheader()->height() << std::endl;
    }
    return count;
}

