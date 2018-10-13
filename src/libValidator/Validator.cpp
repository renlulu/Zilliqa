/*
 * Copyright (c) 2018 Zilliqa
 * This source code is being disclosed to you solely for the purpose of your
 * participation in testing Zilliqa. You may view, compile and run the code for
 * that purpose and pursuant to the protocols and algorithms that are programmed
 * into, and intended by, the code. You may not do anything else with the code
 * without express permission from Zilliqa Research Pte. Ltd., including
 * modifying or publishing the code (or any part of it), and developing or
 * forming another public or private blockchain network. This source code is
 * provided 'as is' and no warranties are given as to title or non-infringement,
 * merchantability or fitness for purpose and, to the extent permitted by law,
 * all liability for your use of the code is disclaimed. Some programs in this
 * code are governed by the GNU General Public License v3.0 (available at
 * https://www.gnu.org/licenses/gpl-3.0.en.html) ('GPLv3'). The programs that
 * are governed by GPLv3.0 are those programs that are located in the folders
 * src/depends and tests/depends and which include a reference to GPLv3 in their
 * program files.
 */

#include <vector>

#include "Validator.h"
#include "libData/AccountData/Account.h"
#include "libMediator/Mediator.h"
#include "libUtils/BitVector.h"

using namespace std;
using namespace boost::multiprecision;

Validator::Validator(Mediator& mediator) : m_mediator(mediator) {}

Validator::~Validator() {}

bool Validator::VerifyTransaction(const Transaction& tran) const {
  vector<unsigned char> txnData;
  tran.SerializeCoreFields(txnData, 0);

  return Schnorr::GetInstance().Verify(txnData, tran.GetSignature(),
                                       tran.GetSenderPubKey());
}

bool Validator::CheckCreatedTransaction(const Transaction& tx,
                                        TransactionReceipt& receipt) const {
  if (LOOKUP_NODE_MODE) {
    LOG_GENERAL(WARNING,
                "Validator::CheckCreatedTransaction not expected to be "
                "called from LookUp node.");
    return true;
  }
  // LOG_MARKER();

  // LOG_GENERAL(INFO, "Tran: " << tx.GetTranID());

  // Check if from account is sharded here
  const PubKey& senderPubKey = tx.GetSenderPubKey();
  Address fromAddr = Account::GetAddressFromPublicKey(senderPubKey);

  // Check if from account exists in local storage
  if (!AccountStore::GetInstance().IsAccountExist(fromAddr)) {
    LOG_GENERAL(INFO, "fromAddr not found: " << fromAddr
                                             << ". Transaction rejected: "
                                             << tx.GetTranID());
    return false;
  }

  // Check if transaction amount is valid
  if (AccountStore::GetInstance().GetBalance(fromAddr) < tx.GetAmount()) {
    LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Insufficient funds in source account!"
                  << " From Account  = 0x" << fromAddr << " Balance = "
                  << AccountStore::GetInstance().GetBalance(fromAddr)
                  << " Debit Amount = " << tx.GetAmount());
    return false;
  }

  return AccountStore::GetInstance().UpdateAccountsTemp(
      m_mediator.m_currentEpochNum, m_mediator.m_node->getNumShards(),
      m_mediator.m_ds->m_mode != DirectoryService::Mode::IDLE, tx, receipt);
}

bool Validator::CheckCreatedTransactionFromLookup(const Transaction& tx) {
  if (LOOKUP_NODE_MODE) {
    LOG_GENERAL(WARNING,
                "Validator::CheckCreatedTransactionFromLookup not expected "
                "to be called from LookUp node.");
    return true;
  }

  // LOG_MARKER();

  // Check if from account is sharded here
  const PubKey& senderPubKey = tx.GetSenderPubKey();
  Address fromAddr = Account::GetAddressFromPublicKey(senderPubKey);
  unsigned int shardId = m_mediator.m_node->GetShardId();
  unsigned int numShards = m_mediator.m_node->getNumShards();
  unsigned int correct_shard_from =
      Transaction::GetShardIndex(fromAddr, numShards);

  if (m_mediator.m_ds->m_mode == DirectoryService::Mode::IDLE) {
    if (correct_shard_from != shardId) {
      LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                "This tx is not sharded to me!"
                    << " From Account  = 0x" << fromAddr
                    << " Correct shard = " << correct_shard_from
                    << " This shard    = " << m_mediator.m_node->GetShardId());
      return false;
      // // Transaction created from the GenTransactionBulk will be rejected
      // // by all shards but one. Next line is commented to avoid this
      // return false;
    }

    if (tx.GetData().size() > 0 && tx.GetToAddr() != NullAddress) {
      unsigned int correct_shard_to =
          Transaction::GetShardIndex(tx.GetToAddr(), numShards);
      if (correct_shard_to != correct_shard_from) {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "The fromShard " << correct_shard_from << " and toShard "
                                   << correct_shard_to
                                   << " is different for the call SC txn");
        return false;
      }
    }
  }

  if (!VerifyTransaction(tx)) {
    LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Signature incorrect: " << fromAddr << ". Transaction rejected: "
                                      << tx.GetTranID());
    return false;
  }

  // Check if from account exists in local storage
  if (!AccountStore::GetInstance().IsAccountExist(fromAddr)) {
    LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
              "fromAddr not found: " << fromAddr << ". Transaction rejected: "
                                     << tx.GetTranID());
    return false;
  }

  // Check if transaction amount is valid
  if (AccountStore::GetInstance().GetBalance(fromAddr) < tx.GetAmount()) {
    LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Insufficient funds in source account!"
                  << " From Account  = 0x" << fromAddr << " Balance = "
                  << AccountStore::GetInstance().GetBalance(fromAddr)
                  << " Debit Amount = " << tx.GetAmount());
    return false;
  }

  return true;
}

template<class Container, class DirectoryBlock> 
bool Validator::CheckBlockCosignature(const DirectoryBlock& block, const Container& commKeys)
{
    LOG_MARKER();

  unsigned int index = 0;
  unsigned int count = 0;

  const vector<bool>& B2 = block.GetB2();
  if (commKeys.size() != B2.size()) {
    LOG_GENERAL(WARNING, "Mismatch: committee size = "
                             << commKeys.size()
                             << ", co-sig bitmap size = " << B2.size());
    return false;
  }

  // Generate the aggregated key
  vector<PubKey> keys;
  for (auto const& kv : commKeys) {
    if (B2.at(index)) {
      keys.emplace_back(get<PubKey>(kv));
      count++;
    }
    index++;
  }

  if (count != ConsensusCommon::NumForConsensus(B2.size())) {
    LOG_GENERAL(WARNING, "Cosig was not generated by enough nodes");
    return false;
  }

  shared_ptr<PubKey> aggregatedKey = MultiSig::AggregatePubKeys(keys);
  if (aggregatedKey == nullptr) {
    LOG_GENERAL(WARNING, "Aggregated key generation failed");
    return false;
  }

  // Verify the collective signature
  vector<unsigned char> serializedHeader;
  block.GetHeader().Serialize(serializedHeader,0);
  block.GetCS1().Serialize(serializedHeader, serializedHeader.size());
  BitVector::SetBitVector(serializedHeader, serializedHeader.size(), block.GetB1());
  if (!Schnorr::GetInstance().Verify(serializedHeader, 0, serializedHeader.size(),
                                     block.GetCS2(), *aggregatedKey)) {
    LOG_GENERAL(WARNING, "Cosig verification failed");
    for (auto& kv : keys) {
      LOG_GENERAL(WARNING, kv);
    }
    return false;
  }

  return true;
}

bool Validator::CheckDirBlocks(const vector<boost::variant<DSBlock,VCBlock,FallbackBlockWShardingStructure>>& dirBlocks,
 const deque<pair<PubKey, Peer>>& initDsComm )
{

  deque<pair<PubKey,Peer>> mutable_ds_comm = initDsComm;

  uint64_t prevdsblocknum = 0;
  uint64_t totalIndex = 0;
  for(const auto& dirBlock : dirBlocks)
  {
    if(typeid(DSBlock)==dirBlock.type())
    {
      const auto& dsblock = get<DSBlock>(dirBlock);
      if(dsblock.GetHeader().GetBlockNum() != prevdsblocknum + 1)
      {
        LOG_GENERAL(WARNING,"DSblocks not in sequence "<<dsblock.GetHeader().GetBlockNum()<<" "<<prevdsblocknum);
        return false;
      }
      
      if(!CheckBlockCosignature(dsblock,mutable_ds_comm))
      {
        LOG_GENERAL(WARNING,"Co-sig verification of ds block "<<prevdsblocknum + 1<<" failed");
        return false;
      }
      prevdsblocknum++;
      m_mediator.m_node->UpdateDSCommiteeComposition(mutable_ds_comm);
      totalIndex++;

    }
    else if(typeid(VCBlock) == dirBlock.type())
    {
      const auto& vcblock = get<VCBlock>(dirBlock);

      if(vcblock.GetHeader().GetVieWChangeDSEpochNo() != prevdsblocknum)
      {
        LOG_GENERAL(WARNING, "VC block ds epoch number does not match the number being processed "<<prevdsblocknum<<" "<<vcblock.GetHeader().GetVieWChangeDSEpochNo());
        return false;
      }
      if(!CheckBlockCosignature(vcblock, mutable_ds_comm))
      {
        LOG_GENERAL(WARNING,"Co-sig verification of vc block in "<<prevdsblocknum<<" failed"<<totalIndex+1);
        return false;
      }
      unsigned int newCandidateLeader = vcblock.GetHeader().GetViewChangeCounter();
      for(unsigned int i = 0 ; i<newCandidateLeader; i++)
      {
        m_mediator.m_node->UpdateDSCommiteeCompositionAfterVC(mutable_ds_comm);
      }
      totalIndex++;
    }
    else if(typeid(FallbackBlockWShardingStructure) == dirBlock.type())
    {
      const auto& fallbackblock = get<FallbackBlockWShardingStructure>(dirBlock).m_fallbackblock;
      const DequeOfShard& shards = get<FallbackBlockWShardingStructure>(dirBlock).m_shards;

      if(fallbackblock.GetHeader().GetFallbackDSEpochNo() != prevdsblocknum)
      {
        LOG_GENERAL(WARNING,"Fallback block ds epoch number does not match the number being processed "<<prevdsblocknum<<" "<<fallbackblock.GetHeader().GetFallbackDSEpochNo());
        return false;
      }

      //Verify Sharding Structure Hash here

      uint32_t shard_id = fallbackblock.GetHeader().GetShardId();


      if(!CheckBlockCosignature(fallbackblock, shards.at(shard_id)))
      {
        LOG_GENERAL(WARNING,"Co-sig verification of fallbackblock in "<<prevdsblocknum<<" failed"<<totalIndex+1);
        return false;
      }
      const PubKey& leaderPubKey = fallbackblock.GetHeader().GetLeaderPubKey();
      const Peer& leaderNetworkInfo = fallbackblock.GetHeader().GetLeaderNetworkInfo();
      m_mediator.m_node->UpdateDSCommitteeAfterFallback(shard_id, leaderPubKey, leaderNetworkInfo, mutable_ds_comm, shards);
      totalIndex++;
    }
    else
    {
      LOG_GENERAL(WARNING,"dirBlock type unexpected ");
    }
  }
  return true;
}
