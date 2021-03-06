/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Common.cpp
 * @author Alex Leverington <nessence@gmail.com>
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include <libdevcore/Guards.h>  // <boost/thread> conflicts with <thread>
#include "Common.h"
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <secp256k1_recovery.h>
#include <secp256k1_sha256.h>
#include <cryptopp/aes.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>
#include <cryptopp/modes.h>
#include <libscrypt/libscrypt.h>
#include <libdevcore/SHA3.h>
#include <libdevcore/RLP.h>
#include <libdevcore/FileSystem.h>
#include "AES.h"
#include "CryptoPP.h"
#include "Exceptions.h"
using namespace std;
using namespace dev;
using namespace dev::crypto;

namespace
{

secp256k1_context const* getCtx()
{
	static std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> s_ctx{
		secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY),
		&secp256k1_context_destroy
	};
	return s_ctx.get();
}

}

bool ECDSA::SignatureStruct::isValid() const noexcept
{
    static const h256 s_max{"0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141"};
    static const h256 s_zero;

    return (v <= 1 && r > s_zero && s > s_zero && r < s_max && s < s_max);
}

bool BLS::SignatureStruct::isValid() const noexcept
{
    return true;
}

/*
BLS::SignatureStruct::SignatureStruct(bytesConstRef bytes) {
    RLP r(bytes);
    RLPList s(r, 2); s >> ((Signature&) *this) >> publicKey;
}
*/
BLS::SignatureStruct::SignatureStruct(RLP const& rlp) {
    RLPList s(rlp, 2); s >> ((Signature&) *this) >> publicKey;
}

void BLS::SignatureStruct::streamRLP(RLPStream& _s) const {
    _s.appendList(2) << Signature(*this) << publicKey;
}


bool ECDSA::SignatureStruct::isZero() const {
    return !s && !r;
}

bool BLS::SignatureStruct::isZero() const {
    return !(bool) (*this);
}

template <> ECDSA::Public dev::toPublic<ECDSA>(ECDSA::Secret const& _secret)
{
    auto* ctx = getCtx();
    secp256k1_pubkey rawPubkey;
    // Creation will fail if the secret key is invalid.
    if (!secp256k1_ec_pubkey_create(ctx, &rawPubkey, _secret.data()))
        return {};
    std::array<byte, 65> serializedPubkey;
    size_t serializedPubkeySize = serializedPubkey.size();
    secp256k1_ec_pubkey_serialize(
            ctx, serializedPubkey.data(), &serializedPubkeySize,
            &rawPubkey, SECP256K1_EC_UNCOMPRESSED
    );
    assert(serializedPubkeySize == serializedPubkey.size());
    // Expect single byte header of value 0x04 -- uncompressed public key.
    assert(serializedPubkey[0] == 0x04);
    // Create the Public skipping the header.
    return ECDSA::Public{&serializedPubkey[1], ECDSA::Public::ConstructFromPointer};
}

template <>
BLS::Public dev::toPublic<BLS>(BLS::Secret const& secret)
{
    return BLS12_381::BonehLynnShacham::generatePublicKey(secret);
}

Address dev::toAddress(Address const& _from, u256 const& _nonce)
{
	return right160(sha3(rlpList(_from, _nonce)));
}

void dev::encrypt(ECDSA::Public const& _k, bytesConstRef _plain, bytes& o_cipher)
{
	bytes io = _plain.toBytes();
	Secp256k1PP::get()->encrypt(_k, io);
	o_cipher = std::move(io);
}

bool dev::decrypt(ECDSA::Secret const& _k, bytesConstRef _cipher, bytes& o_plaintext)
{
	bytes io = _cipher.toBytes();
	Secp256k1PP::get()->decrypt(_k, io);
	if (io.empty())
		return false;
	o_plaintext = std::move(io);
	return true;
}

void dev::encryptECIES(ECDSA::Public const& _k, bytesConstRef _plain, bytes& o_cipher)
{
	encryptECIES(_k, bytesConstRef(), _plain, o_cipher);
}

void dev::encryptECIES(ECDSA::Public const& _k, bytesConstRef _sharedMacData, bytesConstRef _plain, bytes& o_cipher)
{
	bytes io = _plain.toBytes();
    Secp256k1PP::get()->encryptECIES(_k, _sharedMacData, io);
    o_cipher = std::move(io);
}

bool dev::decryptECIES(ECDSA::Secret const& _k, bytesConstRef _cipher, bytes& o_plaintext)
{
    return decryptECIES(_k, bytesConstRef(),  _cipher, o_plaintext);
}

bool dev::decryptECIES(ECDSA::Secret const& _k, bytesConstRef _sharedMacData, bytesConstRef _cipher, bytes& o_plaintext)
{
	bytes io = _cipher.toBytes();
	if (!Secp256k1PP::get()->decryptECIES(_k, _sharedMacData, io))
		return false;
	o_plaintext = std::move(io);
	return true;
}

void dev::encryptSym(ECDSA::Secret const& _k, bytesConstRef _plain, bytes& o_cipher)
{
	// TODO: @alex @subtly do this properly.
    encrypt(KeyPair<ECDSA>(_k).pub(), _plain, o_cipher);
}

bool dev::decryptSym(ECDSA::Secret const& _k, bytesConstRef _cipher, bytes& o_plain)
{
	// TODO: @alex @subtly do this properly.
	return decrypt(_k, _cipher, o_plain);
}

std::pair<bytes, h128> dev::encryptSymNoAuth(SecureFixedHash<16> const& _k, bytesConstRef _plain)
{
	h128 iv(Nonce::get().makeInsecure());
	return make_pair(encryptSymNoAuth(_k, iv, _plain), iv);
}

bytes dev::encryptAES128CTR(bytesConstRef _k, h128 const& _iv, bytesConstRef _plain)
{
	if (_k.size() != 16 && _k.size() != 24 && _k.size() != 32)
		return bytes();
	CryptoPP::SecByteBlock key(_k.data(), _k.size());
	try
	{
		CryptoPP::CTR_Mode<CryptoPP::AES>::Encryption e;
		e.SetKeyWithIV(key, key.size(), _iv.data());
		bytes ret(_plain.size());
		e.ProcessData(ret.data(), _plain.data(), _plain.size());
		return ret;
	}
	catch (CryptoPP::Exception& _e)
	{
		cerr << _e.what() << endl;
		return bytes();
	}
}

bytesSec dev::decryptAES128CTR(bytesConstRef _k, h128 const& _iv, bytesConstRef _cipher)
{
	if (_k.size() != 16 && _k.size() != 24 && _k.size() != 32)
		return bytesSec();
	CryptoPP::SecByteBlock key(_k.data(), _k.size());
	try
	{
		CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption d;
		d.SetKeyWithIV(key, key.size(), _iv.data());
		bytesSec ret(_cipher.size());
		d.ProcessData(ret.writable().data(), _cipher.data(), _cipher.size());
		return ret;
	}
	catch (CryptoPP::Exception& _e)
	{
		cerr << _e.what() << endl;
		return bytesSec();
	}
}

dev::ECDSA::Public dev::recover(dev::ECDSA::Signature const& _sig, h256 const& _message)
{
    int v = _sig[64];
	if (v > 3)
		return {};

	auto* ctx = getCtx();
	secp256k1_ecdsa_recoverable_signature rawSig;
	if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &rawSig, _sig.data(), v))
		return {};

	secp256k1_pubkey rawPubkey;
	if (!secp256k1_ecdsa_recover(ctx, &rawPubkey, &rawSig, _message.data()))
		return {};

	std::array<byte, 65> serializedPubkey;
	size_t serializedPubkeySize = serializedPubkey.size();
	secp256k1_ec_pubkey_serialize(
			ctx, serializedPubkey.data(), &serializedPubkeySize,
			&rawPubkey, SECP256K1_EC_UNCOMPRESSED
	);
	assert(serializedPubkeySize == serializedPubkey.size());
	// Expect single byte header of value 0x04 -- uncompressed public key.
	assert(serializedPubkey[0] == 0x04);
	// Create the Public skipping the header.
    return dev::ECDSA::Public{&serializedPubkey[1], dev::ECDSA::Public::ConstructFromPointer};
}

static const u256 c_secp256k1n("115792089237316195423570985008687907852837564279074904382605163141518161494337");

template <>
dev::ECDSA::Signature dev::sign<ECDSA>(ECDSA::Secret const& _k, h256 const& _hash)
{
	auto* ctx = getCtx();
	secp256k1_ecdsa_recoverable_signature rawSig;
	if (!secp256k1_ecdsa_sign_recoverable(ctx, &rawSig, _hash.data(), _k.data(), nullptr, nullptr))
		return {};

    dev::ECDSA::Signature s;
	int v = 0;
	secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, s.data(), &v, &rawSig);

    dev::ECDSA::SignatureStruct& ss = *reinterpret_cast<dev::ECDSA::SignatureStruct*>(&s);
	ss.v = static_cast<byte>(v);
	if (ss.s > c_secp256k1n / 2)
	{
		ss.v = static_cast<byte>(ss.v ^ 1);
		ss.s = h256(c_secp256k1n - u256(ss.s));
	}
	assert(ss.s <= c_secp256k1n / 2);
	return s;
}

bool dev::verify(ECDSA::Public const& _p, ECDSA::Signature const& _s, h256 const& _hash)
{
    // TODO: Verify w/o recovery (if faster).
    if (!_p)
        return false;
    return _p == recover(_s, _hash);
}

bool dev::verify(BLS::Public const& _publicKey, BLS::Signature const& _signature, h256 const& _hash) { return verify<BLS>(_publicKey, _signature, _hash); }

bytes to8ByteHash(bytes from) { return sha3(from).asBytes();
    bytes x = sha3(from).asBytes();
    for (size_t q = sizeof(h64); q < x.size(); ++q) { x[q % sizeof(h64)] ^= x[q]; }
    x.resize(sizeof(h64));
    return x;
}

    BLS12_381::G1 hashToElement(BLS::Public const& publicKey, h256 const& hash) {
        bytes h = sha3(dev::asBytes(dev::getDefaultDataDirName()) + publicKey.asBytes() + hash.asBytes()).asBytes();
        return BLS12_381::G1::mapToElement(ref(h));
    }

    template <>
    dev::BLS::Signature dev::sign<BLS>(BLS::Secret const& secret, h256 const& hash)
    {
        return BLS12_381::BonehLynnShacham::sign(hashToElement(toPublic<BLS>(secret), hash), secret);
    }

    template <>
    bool dev::verify<BLS>(BLS::Public const& publicKey, BLS::Signature const& signature, h256 const& hash)
    {
        return BLS12_381::BonehLynnShacham::verify(publicKey, hashToElement(publicKey, hash), signature);
    }

bytesSec dev::pbkdf2(string const& _pass, bytes const& _salt, unsigned _iterations, unsigned _dkLen)
{
	bytesSec ret(_dkLen);
	if (CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA256>().DeriveKey(
		ret.writable().data(),
		_dkLen,
		0,
		reinterpret_cast<byte const*>(_pass.data()),
		_pass.size(),
		_salt.data(),
		_salt.size(),
		_iterations
	) != _iterations)
		BOOST_THROW_EXCEPTION(CryptoException() << errinfo_comment("Key derivation failed."));
	return ret;
}

bytesSec dev::scrypt(std::string const& _pass, bytes const& _salt, uint64_t _n, uint32_t _r, uint32_t _p, unsigned _dkLen)
{
	bytesSec ret(_dkLen);
	if (libscrypt_scrypt(
		reinterpret_cast<uint8_t const*>(_pass.data()),
		_pass.size(),
		_salt.data(),
		_salt.size(),
		_n,
		_r,
		_p,
		ret.writable().data(),
		_dkLen
	) != 0)
		BOOST_THROW_EXCEPTION(CryptoException() << errinfo_comment("Key derivation failed."));
	return ret;
}

h256 crypto::kdf(ECDSA::Secret const& _priv, h256 const& _hash)
{
	// H(H(r||k)^h)
	h256 s;
    sha3mac(ECDSA::Secret::random().ref(), _priv.ref(), s.ref());
	s ^= _hash;
	sha3(s.ref(), s.ref());
	
	if (!s || !_hash || !_priv)
		BOOST_THROW_EXCEPTION(InvalidState());
	return s;
}

CommKeys::Secret Nonce::next()
{
	Guard l(x_value);
	if (!m_value)
	{
		m_value = CommKeys::Secret::random();
		if (!m_value)
			BOOST_THROW_EXCEPTION(InvalidState());
	}
	m_value = sha3Secure(m_value.ref());
	return sha3(~m_value);
}

bool ecdh::agree(ECDSA::Secret const& _s, ECDSA::Public const& _r, ECDSA::Secret& o_s) noexcept
{
    auto* ctx = getCtx();
        static_assert(sizeof(_s) == 32, "Invalid Secret type size");
        secp256k1_pubkey rawPubkey;
        std::array<byte, 65> serializedPubKey{{0x04}};
        std::copy(_r.asArray().begin(), _r.asArray().end(), serializedPubKey.begin() + 1);
        if (!secp256k1_ec_pubkey_parse(ctx, &rawPubkey, serializedPubKey.data(), serializedPubKey.size()))
            return false;  // Invalid public key.
        // FIXME: We should verify the public key when constructed, maybe even keep
        //        secp256k1_pubkey as the internal data of Public.
        std::array<byte, 33> compressedPoint;
        if (!secp256k1_ecdh_raw(ctx, compressedPoint.data(), &rawPubkey, _s.data()))
            return false;  // Invalid secret key.
    std::copy(compressedPoint.begin() + 1, compressedPoint.end(), o_s.writable().data());
    return true;
}

bytes ecies::kdf(ECDSA::Secret const& _z, bytes const& _s1, unsigned kdByteLen)
{
	auto reps = ((kdByteLen + 7) * 8) / 512;
	// SEC/ISO/Shoup specify counter size SHOULD be equivalent
	// to size of hash output, however, it also notes that
	// the 4 bytes is okay. NIST specifies 4 bytes.
	std::array<byte, 4> ctr{{0, 0, 0, 1}};
	bytes k;
	secp256k1_sha256_t ctx;
	for (unsigned i = 0; i <= reps; i++)
	{
		secp256k1_sha256_initialize(&ctx);
		secp256k1_sha256_write(&ctx, ctr.data(), ctr.size());
		secp256k1_sha256_write(&ctx, _z.data(), ECDSA::Secret::size);
		secp256k1_sha256_write(&ctx, _s1.data(), _s1.size());
		// append hash to k
		std::array<byte, 32> digest;
		secp256k1_sha256_finalize(&ctx, digest.data());

		k.reserve(k.size() + h256::size);
		move(digest.begin(), digest.end(), back_inserter(k));

		if (++ctr[3] || ++ctr[2] || ++ctr[1] || ++ctr[0])
			continue;
	}

	k.resize(kdByteLen);
	return k;
}

template class KeyPair<ECDSA>;
template class KeyPair<BLS>;
