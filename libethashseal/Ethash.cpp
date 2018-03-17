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
/** @file Ethash.cpp
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include "Ethash.h"
#include <libethereum/Interface.h>
#include <libethcore/ChainOperationParams.h>
#include <libethcore/CommonJS.h>

using namespace std;
using namespace dev;
using namespace eth;

void Ethash::init()
{
	ETH_REGISTER_SEAL_ENGINE(Ethash);
}

StakeModifier Ethash::computeChildStakeModifier(StakeModifier const& parentStakeModifier, StakeKeys::Public const& minerPubKey, StakeKeys::Signature const& minterStakeSig) {
    StakeModifier stakeModifier = dev::sha3(parentStakeModifier.asBytes() + minerPubKey.asBytes() + minterStakeSig.asBytes());
    //clog << "Stake modifier from parent modifier " << parentStakeModifier.hex() << " miner pub key " << minerPubKey.hex() << " miner stake sig " << minterStakeSig.hex() << " = "
      //   << stakeModifier << "\n";
    return stakeModifier;
}

StakeMessage Ethash::computeStakeMessage(StakeModifier const& stakeModifier, u256 timestamp) {
    return dev::sha3(stakeModifier.asBytes() + h256(timestamp));
}

StakeKeys::Signature Ethash::computeStakeSignature(StakeMessage const& message, Secret const& sealerSecretKey) {
    return sign<dev::BLS>(sealerSecretKey, message);
}

bool Ethash::verifyStakeSignature(StakeKeys::Public const& publicKey, StakeKeys::Signature const& signature, StakeMessage const& message) {
    return ::verify<BLS>(publicKey, signature, message);
}

StakeSignatureHash Ethash::computeStakeSignatureHash(StakeKeys::Signature const& stakeSignature) {
    return dev::sha3(stakeSignature);
}

strings Ethash::sealers() const
{
	return {"cpu"};
}

StringHashMap Ethash::jsInfo(BlockHeader const& _bi) const
{
    return { { "stakeModifier", toJS(stakeModifier(_bi)) }, { "publicKey", toJS(publicKey(_bi)) }, { "stakeSignature", toJS(stakeSignature(_bi)) }, { "blockSignature", toJS(blockSignature(_bi)) },
         { "difficulty", toJS(_bi.difficulty()) } };
}

void Ethash::verify(Strictness _s, BlockHeader const& _bi, BlockHeader const& _parent, bytesConstRef _block) const
{
	SealEngineFace::verify(_s, _bi, _parent, _block);

	if (_s != CheckNothingNew)
	{
		if (_bi.difficulty() < chainParams().minimumDifficulty)
			BOOST_THROW_EXCEPTION(InvalidDifficulty() << RequirementError(bigint(chainParams().minimumDifficulty), bigint(_bi.difficulty())) );

		if (_bi.gasLimit() < chainParams().minGasLimit)
			BOOST_THROW_EXCEPTION(InvalidGasLimit() << RequirementError(bigint(chainParams().minGasLimit), bigint(_bi.gasLimit())) );

		if (_bi.gasLimit() > chainParams().maxGasLimit)
			BOOST_THROW_EXCEPTION(InvalidGasLimit() << RequirementError(bigint(chainParams().maxGasLimit), bigint(_bi.gasLimit())) );

		if (_bi.number() && _bi.extraData().size() > chainParams().maximumExtraDataSize)
			BOOST_THROW_EXCEPTION(ExtraDataTooBig() << RequirementError(bigint(chainParams().maximumExtraDataSize), bigint(_bi.extraData().size())) << errinfo_extraData(_bi.extraData()));
	}

//	if (_parent)
	{
		// Check difficulty is correct given the two timestamps.
        auto expected = calculateDifficulty(_bi, _parent);
		auto difficulty = _bi.difficulty();
        //clog << "Difficulty: " << difficulty << " expected: " << expected << " Parent.number: " << _parent.number() << "\n";
		if (difficulty != expected)
			BOOST_THROW_EXCEPTION(InvalidDifficulty() << RequirementError((bigint)expected, (bigint)difficulty));

		auto gasLimit = _bi.gasLimit();
		auto parentGasLimit = _parent.gasLimit();
		if (
			gasLimit < chainParams().minGasLimit ||
			gasLimit > chainParams().maxGasLimit ||
			gasLimit <= parentGasLimit - parentGasLimit / chainParams().gasLimitBoundDivisor ||
			gasLimit >= parentGasLimit + parentGasLimit / chainParams().gasLimitBoundDivisor)
			BOOST_THROW_EXCEPTION(
				InvalidGasLimit()
				<< errinfo_min((bigint)((bigint)parentGasLimit - (bigint)(parentGasLimit / chainParams().gasLimitBoundDivisor)))
				<< errinfo_got((bigint)gasLimit)
				<< errinfo_max((bigint)((bigint)parentGasLimit + parentGasLimit / chainParams().gasLimitBoundDivisor))
			);
	}

	// check it hashes according to proof of work or that it's the genesis block.
    if ((_s == CheckEverything || _s == QuickNonce) && _bi.parentHash() && !verifySeal(_bi, _parent))
	{
		InvalidBlockNonce ex;
//		ex << errinfo_nonce(nonce(_bi));
//		ex << errinfo_mixHash(mixHash(_bi));
//		ex << errinfo_seedHash(seedHash(_bi));
//		EthashProofOfWork::Result er = EthashAux::eval(seedHash(_bi), _bi.hash(WithoutSeal), nonce(_bi));
//		ex << errinfo_ethashResult(make_tuple(er.value, er.mixHash));
		ex << errinfo_hash256(_bi.hash(WithoutSeal));
		ex << errinfo_difficulty(_bi.difficulty());
//		ex << errinfo_target(boundary(_bi));
		BOOST_THROW_EXCEPTION(ex);
	}
/*	else if (_s == QuickNonce && _bi.parentHash() && !quickVerifySeal(_bi))
	{
		InvalidBlockNonce ex;
		ex << errinfo_hash256(_bi.hash(WithoutSeal));
		ex << errinfo_difficulty(_bi.difficulty());
		ex << errinfo_nonce(nonce(_bi));
		BOOST_THROW_EXCEPTION(ex);
    }*/
}

void Ethash::verifyTransaction(ImportRequirements::value _ir, TransactionBase const& _t, BlockHeader const& _header, u256 const& _startGasUsed) const
{
	SealEngineFace::verifyTransaction(_ir, _t, _header, _startGasUsed);

	if (_ir & ImportRequirements::TransactionSignatures)
	{
        int chainID = chainParams().chainID;
        _t.checkChainId(chainID);
	}
	if (_ir & ImportRequirements::TransactionBasic && _t.baseGasRequired(evmSchedule(_header.number())) > _t.gas())
		BOOST_THROW_EXCEPTION(OutOfGasIntrinsic() << RequirementError((bigint)(_t.baseGasRequired(evmSchedule(_header.number()))), (bigint)_t.gas()));

	// Avoid transactions that would take us beyond the block gas limit.
	if (_startGasUsed + (bigint)_t.gas() > _header.gasLimit())
		BOOST_THROW_EXCEPTION(BlockGasLimitReached() << RequirementError((bigint)(_header.gasLimit() - _startGasUsed), (bigint)_t.gas()));
}

u256 Ethash::childGasLimit(BlockHeader const& _bi, u256 const& _gasFloorTarget) const
{
	u256 gasFloorTarget = _gasFloorTarget == Invalid256 ? 3141562 : _gasFloorTarget;
	u256 gasLimit = _bi.gasLimit();
	u256 boundDivisor = chainParams().gasLimitBoundDivisor;
	if (gasLimit < gasFloorTarget)
		return min<u256>(gasFloorTarget, gasLimit + gasLimit / boundDivisor - 1);
	else
		return max<u256>(gasFloorTarget, gasLimit - gasLimit / boundDivisor + 1 + (_bi.gasUsed() * 6 / 5) / boundDivisor);
}

u256 Ethash::calculateDifficulty(BlockHeader const& _bi, BlockHeader const& parent) const {
    //clog << "Parent.number: " << parent.number() << " ";
    return calculateDifficulty(_bi, parent.timestamp(), parent.difficulty());
}

u256 Ethash::calculateDifficulty(BlockHeader const& _bi, bigint const& parentTimeStamp, bigint const& parentDifficulty) const
{
	if (!_bi.number())
		throw GenesisBlockCannotBeCalculated();
	auto const& minimumDifficulty = chainParams().minimumDifficulty;

    bigint const timestampDiff = bigint(_bi.timestamp()) - bigint(parentTimeStamp);
    bigint const adjFactor = max<bigint>(1 - timestampDiff / 9, -99); // Byzantium-era difficulty adjustment

    bigint target = parentDifficulty + parentDifficulty / 2048 * adjFactor;
    //clog << "Parent timestamp: " << parentTimeStamp << " bi.timestamp " << _bi.timestamp() << " timestampDiff: " << timestampDiff << " parent diff: " << parentDifficulty << " target: " << target << "\n";
    return u256(min<bigint>(max<bigint>(minimumDifficulty, target), std::numeric_limits<u256>::max()));
}

void Ethash::populateFromParent(BlockHeader& _bi, BlockHeader const& _parent)
{
	SealEngineFace::populateFromParent(_bi, _parent);
    _bi.setGasLimit(childGasLimit(_parent));
}

h256 Ethash::boundary(BlockHeader const& _bi, u256 const& balance) const {
    auto d = _bi.difficulty();
    return d ? (h256)u256(((bigint(1) << 256)/ d) * balance) : h256();
}

bool Ethash::verifySeal(BlockHeader const& _bi, BlockHeader const& m_parent) const
{
    if (_bi.number() != m_parent.number() + 1)
        return false;
    //clog << "verifying signature for block " << _bi.number() << " (" << _bi.hash(WithoutSeal).hex() << ")"
      //     << " with parent " << m_parent.number() << " (" << m_parent.hash(WithoutSeal).hex() << ")";
    Address minterAddress = publicToAddress<BLS::Public>(publicKey(_bi));
    u256 minterBalance = m_balanceRetriever(minterAddress, (BlockNumber) (_bi.number() - 1));
    StakeKeys::Signature stakeSig = stakeSignature(_bi);
    bool meetsBounds = computeStakeSignatureHash(stakeSig) <= boundary(_bi, minterBalance);
    bool modifierCorrect = stakeModifier(_bi) == computeChildStakeModifier(stakeModifier(m_parent), publicKey(_bi), stakeSig);
    bool stakeSignatureVerified = verifyStakeSignature(publicKey(_bi), stakeSig, computeStakeMessage(stakeModifier(m_parent), _bi.timestamp()));
    bool blockSignatureVerified = ::verify<BLS>(publicKey(_bi), blockSignature(_bi), _bi.hash(WithoutSeal));
    return meetsBounds && modifierCorrect && blockSignatureVerified && stakeSignatureVerified;
}

void Ethash::generateSeal(BlockHeader _bi, BlockHeader const& parent)
{
    clog << " generate seal for " << _bi.number() << " m_parent.nubmer: " << parent.number() << "\n";
    if (!m_generating) {
        Guard l(m_submitLock);
        m_sealing = _bi;
        if (sealThread.joinable()) sealThread.join();
        m_generating = true;
        sealThread = std::thread([parent, this](){
            u256 timestamp = minimalTimeStamp(parent);
            while (m_generating) {
                u256 currentTime;
                while (m_generating && (timestamp > (currentTime = utcTime()))) this_thread::sleep_for(chrono::milliseconds(100));
                if (!m_generating) break;

//                clog << "Timestamp: " << timestamp << " keypairs = " << m_keyPairs.size() << " parent no: " << parent.number() << "\n";
                m_sealing.setTimestamp(timestamp);
                m_sealing.setDifficulty(calculateDifficulty(m_sealing, parent));
                for (auto kp: m_keyPairs) {
                    u256 balance = m_balanceRetriever(kp.address(), (BlockNumber) (m_sealing.number() - 1));
                    StakeKeys::Signature r = computeStakeSignature(computeStakeMessage(stakeModifier(parent), timestamp), kp.secret());
                    if (computeStakeSignatureHash(r) <= boundary(m_sealing, balance)) {
                        std::unique_lock<Mutex> l(m_submitLock);
                        setStakeModifier(m_sealing, computeChildStakeModifier(stakeModifier(parent), kp.pub(), r));
                        setPublicKey(m_sealing, kp.pub());
                        setStakeSignature(m_sealing, r);
                        setBlockSignature(m_sealing, sign<BLS>(kp.secret(), m_sealing.hash(WithoutSeal)));

                        if (m_onSealGenerated)
                        {
                            clog << "seal generated: " << m_sealing.number() << "\n";
                            assert(verifySeal(m_sealing, parent));

                            RLPStream ret;
                            m_sealing.streamRLP(ret);
                            l.unlock();
                            m_onSealGenerated(ret.out());
                        }
                        m_generating  = false;
                        return true;
                    };
                }
                timestamp += 1;
          //      if (m_generating && lastPaused) this_thread::sleep_for(chrono::milliseconds(500));
            }
            return true;
        });
    }
}

bool Ethash::shouldSeal(Interface*)
{
	return true;
}
