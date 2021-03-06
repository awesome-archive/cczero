/*
  This file is part of Chinese Chess Zero.
  Copyright (C) 2018 The CCZero Authors

  Chinese Chess Zero is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Chinese Chess Zero is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Chinese Chess Zero.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "mcts/node.h"
#include "neural/encoder.h"
#include "neural/network.h"
#include "utils/hashcat.h"

namespace cczero {

/////////////////////////////////////////////////////////////////////////
// Node garbage collector
/////////////////////////////////////////////////////////////////////////

namespace {
// Periodicity of garbage collection, milliseconds.
const int kGCIntervalMs = 100;

// Every kGCIntervalMs milliseconds release nodes in a separate GC thread.
class NodeGarbageCollector {
   public:
    NodeGarbageCollector() : gc_thread_([this]() { Worker(); }) {}

    // Takes ownership of a subtree, to dispose it in a separate thread when
    // it has time.
    void AddToGcQueue(std::unique_ptr<Node> node) {
        if (!node) return;
        Mutex::Lock lock(gc_mutex_);
        subtrees_to_gc_.emplace_back(std::move(node));
    }

    ~NodeGarbageCollector() {
        // Flips stop flag and waits for a worker thread to stop.
        stop_ = true;
        gc_thread_.join();
    }

   private:
    void GarbageCollect() {
        while (!stop_) {
            // Node will be released in destructor when mutex is not locked.
            std::unique_ptr<Node> node_to_gc;
            {
                // Lock the mutex and move last subtree from subtrees_to_gc_
                // into node_to_gc.
                Mutex::Lock lock(gc_mutex_);
                if (subtrees_to_gc_.empty()) return;
                node_to_gc = std::move(subtrees_to_gc_.back());
                subtrees_to_gc_.pop_back();
            }
        }
    }

    void Worker() {
        while (!stop_) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kGCIntervalMs));
            GarbageCollect();
        };
    }

    mutable Mutex gc_mutex_;
    std::vector<std::unique_ptr<Node>> subtrees_to_gc_ GUARDED_BY(gc_mutex_);

    // When true, Worker() should stop and exit.
    volatile bool stop_ = false;
    std::thread gc_thread_;
};  // namespace

NodeGarbageCollector gNodeGc;
}  // namespace

/////////////////////////////////////////////////////////////////////////
// Edge
/////////////////////////////////////////////////////////////////////////

Move Edge::GetMove(bool as_opponent) const {
    if (!as_opponent) return move_;
    Move m = move_;
    m.Mirror();
    return m;
}

std::string Edge::DebugString() const {
    std::ostringstream oss;
    oss << "Move: " << move_.as_string() << " P:" << p_;
    return oss.str();
}

/////////////////////////////////////////////////////////////////////////
// EdgeList
/////////////////////////////////////////////////////////////////////////

EdgeList::EdgeList(MoveList moves)
    : edges_(std::make_unique<Edge[]>(moves.size())), size_(moves.size()) {
    auto* edge = edges_.get();
    for (auto move : moves) edge++->SetMove(move);
}

/////////////////////////////////////////////////////////////////////////
// Node
/////////////////////////////////////////////////////////////////////////

Node* Node::CreateSingleChildNode(Move move) {
    assert(!edges_);
    assert(!child_);
    edges_ = EdgeList({move});
    child_ = std::make_unique<Node>(this, 0);
    return child_.get();
}

void Node::CreateEdges(const MoveList& moves) {
    assert(!edges_);
    assert(!child_);
    edges_ = EdgeList(moves);
}

Node::ConstIterator Node::Edges() const { return {edges_, &child_}; }
Node::Iterator Node::Edges() { return {edges_, &child_}; }

float Node::GetVisitedPolicy() const { return visited_policy_; }

Edge* Node::GetEdgeToNode(const Node* node) const {
    assert(node->parent_ == this);
    assert(node->index_ < edges_.size());
    return &edges_[node->index_];
}

std::string Node::DebugString() const {
    std::ostringstream oss;
    oss << " Term:" << is_terminal_ << " This:" << this << " Parent:" << parent_
        << " Index:" << index_ << " Child:" << child_.get()
        << " Sibling:" << sibling_.get() << " Q:" << q_ << " N:" << n_
        << " N_:" << n_in_flight_ << " Edges:" << edges_.size();
    return oss.str();
}

void Node::MakeTerminal(GameResult result) {
    is_terminal_ = true;
    q_ = (result == GameResult::DRAW) ? 0.0f : 1.0f;
}

bool Node::TryStartScoreUpdate() {
    if (n_ == 0 && n_in_flight_ > 0) return false;
    ++n_in_flight_;
    return true;
}

void Node::CancelScoreUpdate() { --n_in_flight_; }

void Node::FinalizeScoreUpdate(float v) {
    // Recompute Q.
    q_ += (v - q_) / (n_ + 1);
    // If first visit, update parent's sum of policies visited at least once.
    if (n_ == 0 && parent_ != nullptr) {
        parent_->visited_policy_ += parent_->edges_[index_].GetP();
    }
    // Increment N.
    ++n_;
    // Decrement virtual loss.
    --n_in_flight_;
}

void Node::UpdateMaxDepth(int depth) {
    if (depth > max_depth_) max_depth_ = depth;
}

bool Node::UpdateFullDepth(uint16_t* depth) {
    // TODO(crem) If this function won't be needed, consider also killing
    //            ChildNodes/NodeRange/Nodes_Iterator.
    if (full_depth_ > *depth) return false;
    for (Node* child : ChildNodes()) {
        if (*depth > child->full_depth_) *depth = child->full_depth_;
    }
    if (*depth >= full_depth_) {
        full_depth_ = ++*depth;
        return true;
    }
    return false;
}

Node::NodeRange Node::ChildNodes() const { return child_.get(); }

void Node::ReleaseChildren() { gNodeGc.AddToGcQueue(std::move(child_)); }

void Node::ReleaseChildrenExceptOne(Node* node_to_save) {
    // Stores node which will have to survive (or nullptr if it's not found).
    std::unique_ptr<Node> saved_node;
    // Pointer to unique_ptr, so that we could move from it.
    for (std::unique_ptr<Node>* node = &child_; *node;
         node = &(*node)->sibling_) {
        // If current node is the one that we have to save.
        if (node->get() == node_to_save) {
            // Kill all remaining siblings.
            gNodeGc.AddToGcQueue(std::move((*node)->sibling_));
            // Save the node, and take the ownership from the unique_ptr.
            saved_node = std::move(*node);
            break;
        }
    }
    // Make saved node the only child. (kills previous siblings).
    gNodeGc.AddToGcQueue(std::move(child_));
    child_ = std::move(saved_node);
}

namespace {
// Reverse bits in every byte of a number
uint64_t ReverseBitsInBytes(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
    return v;
}
}  // namespace

V3TrainingData Node::GetV3TrainingData(GameResult game_result,
                                       const PositionHistory& history) const {
    V3TrainingData result;

    // Set version.
    result.version = 3;

    // Populate probabilities.
    float total_n = static_cast<float>(
        GetN() - 1);  // First visit was expansion of it inself.
    std::memset(result.probabilities, 0, sizeof(result.probabilities));
    for (const auto& child : Edges()) {
        result.probabilities[child.edge()->GetMove().as_nn_index()] =
            child.GetN() / total_n;
    }

    // Populate planes.
    InputPlanes planes = EncodePositionForNN(history, 8);
    int plane_idx = 0;
    for (auto& plane : result.planes) {
        plane = ReverseBitsInBytes(planes[plane_idx++].mask);
    }

    const auto& position = history.Last();
    // Populate castlings.
    result.castling_us_ooo = position.CanCastle(Position::WE_CAN_OOO) ? 1 : 0;
    result.castling_us_oo = position.CanCastle(Position::WE_CAN_OO) ? 1 : 0;
    result.castling_them_ooo =
        position.CanCastle(Position::THEY_CAN_OOO) ? 1 : 0;
    result.castling_them_oo = position.CanCastle(Position::THEY_CAN_OO) ? 1 : 0;

    // Other params.
    result.side_to_move = position.IsBlackToMove() ? 1 : 0;
    result.move_count = 0;
    result.rule50_count = position.GetNoCapturePly();

    // Game result.
    if (game_result == GameResult::WHITE_WON) {
        result.result = position.IsBlackToMove() ? -1 : 1;
    } else if (game_result == GameResult::BLACK_WON) {
        result.result = position.IsBlackToMove() ? 1 : -1;
    } else {
        result.result = 0;
    }

    return result;
}

/////////////////////////////////////////////////////////////////////////
// EdgeAndNode
/////////////////////////////////////////////////////////////////////////

std::string EdgeAndNode::DebugString() const {
    if (!edge_) return "(no edge)";
    return edge_->DebugString() + " " +
           (node_ ? node_->DebugString() : "(no node)");
}

/////////////////////////////////////////////////////////////////////////
// NodeTree
/////////////////////////////////////////////////////////////////////////

void NodeTree::MakeMove(Move move) {
    if (HeadPosition().IsBlackToMove()) move.Mirror();

    Node* new_head = nullptr;
    for (auto& n : current_head_->Edges()) {
        if (n.GetMove() == move) {
            new_head = n.GetOrSpawnNode(current_head_);
            break;
        }
    }
    current_head_->ReleaseChildrenExceptOne(new_head);
    current_head_ =
        new_head ? new_head : current_head_->CreateSingleChildNode(move);
    history_.Append(move);
}

void NodeTree::TrimTreeAtHead() {
    auto tmp = std::move(current_head_->sibling_);
    // Send dependent nodes for GC instead of destroying them immediately.
    gNodeGc.AddToGcQueue(std::move(current_head_->child_));
    *current_head_ = Node(current_head_->GetParent(), current_head_->index_);
    current_head_->sibling_ = std::move(tmp);
}

void NodeTree::ResetToPosition(const std::string& starting_fen,
                               const std::vector<Move>& moves) {
    ChessBoard starting_board;
    int no_capture_ply;
    int full_moves;
    starting_board.SetFromFen(starting_fen, &no_capture_ply, &full_moves);
    if (gamebegin_node_ && history_.Starting().GetBoard() != starting_board) {
        // Completely different position.
        DeallocateTree();
    }

    if (!gamebegin_node_) {
        gamebegin_node_ = std::make_unique<Node>(nullptr, 0);
    }

    history_.Reset(starting_board, no_capture_ply,
                   full_moves * 2 - (starting_board.flipped() ? 1 : 2));

    Node* old_head = current_head_;
    current_head_ = gamebegin_node_.get();
    bool seen_old_head = (gamebegin_node_.get() == old_head);
    for (const auto& move : moves) {
        MakeMove(move);
        if (old_head == current_head_) seen_old_head = true;
    }

    // If we didn't see old head, it means that new position is shorter.
    // As we killed the search tree already, trim it to redo the search.
    if (!seen_old_head) {
        assert(!current_head_->sibling_);
        TrimTreeAtHead();
    }
}

void NodeTree::DeallocateTree() {
    // Same as gamebegin_node_.reset(), but actual deallocation will happen in
    // GC thread.
    gNodeGc.AddToGcQueue(std::move(gamebegin_node_));
    gamebegin_node_ = nullptr;
    current_head_ = nullptr;
}

}  // namespace cczero
