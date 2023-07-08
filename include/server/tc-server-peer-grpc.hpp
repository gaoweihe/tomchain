#include "spdlog/spdlog.h"

#include <grpcpp/grpcpp.h>
#include "tc-server-peer.grpc.pb.h"

#include "tc-server.hpp"

namespace tomchain
{

    class TcPeerConsensusImpl final : public TcPeerConsensus::CallbackService
    {

    public:
        std::shared_ptr<TcServer> tc_server_;

    public:
        /**
         * @brief Server heartbeats with peers.
         *
         * @param context RPC context.
         * @param request RPC request.
         * @param response RPC response.
         * @return grpc::Status RPC status.
         */
        grpc::ServerUnaryReactor *SPHeartbeat(
            grpc::CallbackServerContext *context,
            const SPHeartbeatRequest *request,
            SPHeartbeatResponse *response) override
        {
            spdlog::debug("gRPC(SPHeartbeat) starts");

            uint32_t peer_id = request->id();

            response->set_status(0);

            grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        /**
         * @brief Server relays votes to its peer.
         *
         * @param context RPC context.
         * @param request RPC request.
         * @param response RPC response.
         * @return grpc::Status RPC status.
         */
        grpc::ServerUnaryReactor *RelayVote(
            grpc::CallbackServerContext *context,
            const RelayVoteRequest *request,
            RelayVoteResponse *response) override
        {
            EASY_FUNCTION("RelayVote_rsp");
            spdlog::debug("gRPC(RelayVote) starts");

            uint32_t peer_id = request->id();
            auto req_votes = request->votes();

            for (auto iter = req_votes.begin(); iter != req_votes.end(); iter++)
            {
                // deserialize relayed votes
                msgpack::sbuffer des_b = stringToSbuffer(*iter);
                auto oh = msgpack::unpack(des_b.data(), des_b.size());
                auto vote = oh->as<std::shared_ptr<BlockVote>>();
                const uint64_t block_id = vote->block_id_;

                // add to local block vote vector
                spdlog::trace("RelayVote: add to local block vote vector");
                BlockCHM::accessor pb_accessor;
                bool is_found = tc_server_->pending_blks.find(pb_accessor, block_id);
                if (!is_found)
                {
                    spdlog::error("RelayVote: block not found"); 
                    continue;
                }
                pb_accessor->second->votes_.insert(
                    std::make_pair(
                        vote->voter_id_,
                        vote));

                // check if vote enough
                if (pb_accessor->second->is_vote_enough((*::conf_data)["client-count"]))
                {
                    pb_accessor->second->merge_votes((*::conf_data)["client-count"]);

                    // insert block to committed
                    spdlog::trace("RelayVote: insert block to committed");
                    BlockCHM::accessor cb_accessor;
                    tc_server_->committed_blks.insert(
                        cb_accessor,
                        block_id);
                    cb_accessor->second = pb_accessor->second;

                    // insert block to bcast commit
                    spdlog::trace("RelayVote: insert block to bcast commit");
                    for (
                        auto iter = tc_server_->bcast_commit_blocks.begin(); 
                        iter != tc_server_->bcast_commit_blocks.end(); 
                        iter++
                    ) {
                        iter->second->push(cb_accessor->second);
                    }

                    // remove block from pending
                    tc_server_->pending_blks.erase(pb_accessor);

                    cb_accessor.release();
                }

                pb_accessor.release(); 
            }

            response->set_status(0);

            grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        /**
         * @brief Server relays new pending blocks to its peer.
         *
         * @param context RPC context.
         * @param request RPC request.
         * @param response RPC response.
         * @return grpc::Status RPC status.
         */
        grpc::ServerUnaryReactor *RelayBlock(
            grpc::CallbackServerContext *context,
            const RelayBlockRequest *request,
            RelayBlockResponse *response) override
        {
            EASY_FUNCTION("RelayBlock_rsp");
            spdlog::debug("gRPC(RelayBlock) starts");

            uint32_t peer_id = request->id();
            auto req_blocks = request->blocks();

            for (auto iter = req_blocks.begin(); iter != req_blocks.end(); iter++)
            {
                // deserialize relayed blocks
                spdlog::trace("RelayBlock: deserialize relayed blocks");
                msgpack::sbuffer des_b = stringToSbuffer(*iter);
                auto oh = msgpack::unpack(des_b.data(), des_b.size());
                auto block = oh->as<std::shared_ptr<Block>>();

                // store block locally
                spdlog::trace("RelayBlock: store block locally");
                BlockCHM::accessor accessor;
                tc_server_->pending_blks.insert(accessor, block->header_.id_);
                accessor->second = block;
                accessor.release(); 
            }

            response->set_status(0);

            grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        /**
         * @brief Server broadcast commit to its peer.
         *
         * @param context RPC context.
         * @param request RPC request.
         * @param response RPC response.
         * @return grpc::Status RPC status.
         */
        grpc::ServerUnaryReactor *SPBcastCommit(
            grpc::CallbackServerContext *context,
            const SPBcastCommitRequest *request,
            SPBcastCommitResponse *response) override
        {
            EASY_FUNCTION("SPBcastCommit");
            spdlog::debug("gRPC(SPBcastCommit) starts");

            uint32_t peer_id = request->id();
            auto req_blocks = request->blocks();
            spdlog::trace("SPBcastCommit: req_blocks size: {}", req_blocks.size()); 

            for (auto iter = req_blocks.begin(); iter != req_blocks.end(); iter++)
            {
                // deserialize bcasted blocks
                spdlog::trace("SPBcastCommit: deserialize bcasted blocks"); 
                msgpack::sbuffer des_b = stringToSbuffer(*iter);
                auto oh = msgpack::unpack(des_b.data(), des_b.size());
                auto block = oh->as<std::shared_ptr<Block>>();

                // remove pending block
                spdlog::trace("SPBcastCommit: remove pending block");
                BlockCHM::accessor pb_accessor;
                bool is_found = tc_server_->pending_blks.find(
                    pb_accessor, block->header_.id_); 
                if (!is_found)
                {
                    spdlog::error("SPBcastCommit: block not found"); 
                }

                // insert into committed blocks
                spdlog::trace("insert into committed blocks");
                BlockCHM::accessor cb_accessor;
                tc_server_->committed_blks.insert(
                    cb_accessor,
                    block->header_.id_);
                cb_accessor->second = block;

                tc_server_->pending_blks.erase(pb_accessor);

                cb_accessor.release();
                pb_accessor.release(); 
            }

            response->set_status(0);

            grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }
    };

    grpc::Status TcServer::SPHeartbeat(uint64_t target_server_id)
    {
        SPHeartbeatRequest request;
        request.set_id(this->server_id);

        SPHeartbeatResponse response;

        grpc::ClientContext context;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;

        grpc::Status status;
        grpc_peer_client_stub_.find(target_server_id)->second->async()->SPHeartbeat(&context, &request, &response, [&mu, &cv, &done, &status](grpc::Status s)
                                                                                    {
                status = std::move(s);
                std::lock_guard<std::mutex> lock(mu);
                done = true;
                cv.notify_one(); });

        std::unique_lock<std::mutex> lock(mu);
        while (!done)
        {
            cv.wait(lock);
        }

        spdlog::trace("gRPC(SPHeartbeat): {}:{}",
                      status.error_code(),
                      status.error_message());

        return status;
    }

    grpc::Status TcServer::RelayVote(uint64_t target_server_id)
    {
        EASY_FUNCTION("RelayVote_req");

        RelayVoteRequest request;
        request.set_id(this->server_id);

        std::shared_ptr<BlockVote> vote;
        while (relay_votes.find(target_server_id)->second->try_pop(vote))
        {
            // serialize vote
            msgpack::sbuffer b;
            msgpack::pack(b, vote);
            std::string ser_vote = sbufferToString(b);

            // add to relayed vote vector
            request.add_votes(ser_vote);
        }

        RelayVoteResponse response;

        grpc::ClientContext context;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;

        grpc::Status status;
        grpc_peer_client_stub_.find(target_server_id)->second->async()->RelayVote(&context, &request, &response, [&mu, &cv, &done, &status](grpc::Status s)
                                                                                  {
                status = std::move(s);
                std::lock_guard<std::mutex> lock(mu);
                done = true;
                cv.notify_one(); });

        std::unique_lock<std::mutex> lock(mu);
        while (!done)
        {
            cv.wait(lock);
        }

        spdlog::trace("gRPC(RelayVote): {}:{}",
                      status.error_code(),
                      status.error_message());

        return status;
    }

    grpc::Status TcServer::RelayBlock(uint64_t target_server_id)
    {
        EASY_FUNCTION("RelayBlock_req");

        RelayBlockRequest request;
        request.set_id(this->server_id);

        std::shared_ptr<Block> block;
        while (relay_blocks.find(target_server_id)->second->try_pop(block))
        {
            // serialize block
            msgpack::sbuffer b;
            msgpack::pack(b, block);
            std::string ser_block = sbufferToString(b);

            // add to relayed block vector
            request.add_blocks(ser_block);
        }

        RelayBlockResponse response;

        grpc::ClientContext context;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;

        grpc::Status status;
        grpc_peer_client_stub_.find(target_server_id)->second->async()->RelayBlock(&context, &request, &response, [&mu, &cv, &done, &status](grpc::Status s)
                                                                                   {
                status = std::move(s);
                std::lock_guard<std::mutex> lock(mu);
                done = true;
                cv.notify_one(); });

        std::unique_lock<std::mutex> lock(mu);
        while (!done)
        {
            cv.wait(lock);
        }

        spdlog::trace("gRPC(RelayBlock): {}:{}",
                      status.error_code(),
                      status.error_message());

        return status;
    }

    grpc::Status TcServer::SPBcastCommit(uint64_t target_server_id)
    {
        SPBcastCommitRequest request;
        request.set_id(this->server_id);

        std::shared_ptr<Block> block;
        while (bcast_commit_blocks.find(target_server_id)->second->try_pop(block))
        {
            // serialize block
            msgpack::sbuffer b;
            msgpack::pack(b, block);
            std::string ser_block = sbufferToString(b);

            // add to bcast block vector
            request.add_blocks(ser_block);
        }

        SPBcastCommitResponse response;

        grpc::ClientContext context;
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;

        grpc::Status status;
        grpc_peer_client_stub_.find(target_server_id)->second->async()->SPBcastCommit(&context, &request, &response, [&mu, &cv, &done, &status](grpc::Status s)
                                                                                      {
                status = std::move(s);
                std::lock_guard<std::mutex> lock(mu);
                done = true;
                cv.notify_one(); });

        std::unique_lock<std::mutex> lock(mu);
        while (!done)
        {
            cv.wait(lock);
        }

        spdlog::trace("gRPC(SPBcastCommit): {}:{}",
                      status.error_code(),
                      status.error_message());

        return status;
    }

}