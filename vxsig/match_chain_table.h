// Copyright 2011-2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Contains classes to work with chains of matches as generated by running
// BinDiff on a set of binaries. For a more detailed description and a high-
// level overview over the malware processing pipeline, see go/zynamics.
//
// Matches are stored in column in a MatchChainTable with each column mapping
// memory addresses to indices for the respective match type (namely function,
// basic block or instruction match).
// An ASCII-UML representation of this object composition looks like this
// (with <>-1--n- representing a one to many relationship):
//
// MatchChainTable <>-1--n- MatchChainColumn
//                           <> <> <>
//                            |  1  |
//   +-n--------------------1-+  |  +-1----------------n-+
//   |                           n                       |
// MatchedFunction <>-1--n- MatchedBasicBlock <>-1--n- MatchedInstruction
//  <>                       <>                         <>
//   |                        1                          |
//   |                        |                          |
//   |                        1                          |
//   +-1-----------------1- MatchedMemoryAddress -1----1-+

#ifndef VXSIG_MATCH_CHAIN_TABLE_H_
#define VXSIG_MATCH_CHAIN_TABLE_H_

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "third_party/zynamics/binexport/binexport2.pb.h"
#include "third_party/zynamics/binexport/util/status.h"
#include "vxsig/binexport_reader.h"
#include "vxsig/types.h"
#include "vxsig/vxsig.pb.h"

namespace security {
namespace vxsig {

// Represents a single BinDiff match between two binaries.
struct MatchedMemoryAddress {
  explicit MatchedMemoryAddress(const MemoryAddressPair& from_match);

  // The primary address is used in MatchCompare<MatchEntityT> which is used
  // in const context, hence the const here.
  const MemoryAddress address = 0;
  const MemoryAddress address_in_next = 0;

  Ident id = 0;
};

// A functor used to compare the actual match contained in a MatchFunction or
// MatchedBasicBlock by their primary address.
template <typename MatchEntityT>
struct MatchCompare {
  bool operator()(MatchEntityT* first, MatchEntityT* second) const {
    return first->match.address < second->match.address;
  }
};

// Represents an instruction that has been matched by BinDiff. The associated
// instruction bytes and disassembly only get populated if the instruction is
// part of a match chain.
struct MatchedInstruction {
  explicit MatchedInstruction(const MemoryAddressPair& from_match);

  MatchedMemoryAddress match;

  std::string raw_instruction_bytes;
  std::string disassembly;
  Immediates immediates;
};

using MatchedInstructions =
    std::set<MatchedInstruction*, MatchCompare<MatchedInstruction>>;

struct MatchedBasicBlock {
  explicit MatchedBasicBlock(const MemoryAddressPair& from_match);

  MatchedMemoryAddress match;
  MatchedInstructions instructions;

  // Weight used for signature trimming, see RawSignature::Piece::weight.
  int weight = 0;
};

using MatchedBasicBlocks =
    std::set<MatchedBasicBlock*, MatchCompare<MatchedBasicBlock>>;

struct MatchedFunction {
  explicit MatchedFunction(const MemoryAddressPair& from_match);

  MatchedMemoryAddress match;
  MatchedBasicBlocks basic_blocks;
  BinExport2::CallGraph::Vertex::Type type =
      BinExport2::CallGraph::Vertex::NORMAL;
};

// This class represents a single column in a table of match chains. Match
// chains result from running BinDiff sequentially on a set of binaries (for
// example, A vs. B vs. C, etc.), and trying to find matches that are present
// across all binaries.
class MatchChainColumn {
 public:
  // Primary index mapping memory addresses in binaries to their respective
  // match objects. Owns the objects.
  using FunctionAddressIndex =
      std::map<MemoryAddress, std::unique_ptr<MatchedFunction>>;
  using BasicBlockAddressIndex =
      std::map<MemoryAddress, std::unique_ptr<MatchedBasicBlock>>;
  using InstructionAddressIndex =
      std::map<MemoryAddress, std::unique_ptr<MatchedInstruction>>;

  // Secondary index to support fast lookups by an artificial match identifier.
  using FunctionIdentIndex = std::map<Ident, MatchedFunction*>;
  using BasicBlockIdentIndex = std::map<Ident, MatchedBasicBlock*>;

  MatchChainColumn() = default;
  MatchChainColumn(const MatchChainColumn&) = delete;
  MatchChainColumn& operator=(const MatchChainColumn&) = delete;

  // Inserts a new function match to the table. If the function was filtered
  // this function returns nullptr.
  MatchedFunction* InsertFunctionMatch(const MemoryAddressPair& match);

  // Inserts a new basic block match to the table.
  MatchedBasicBlock* InsertBasicBlockMatch(MatchedFunction* function,
                                           const MemoryAddressPair& match);

  // Inserts a new instruction match to the table.
  MatchedInstruction* InsertInstructionMatch(MatchedBasicBlock* basic_block,
                                             const MemoryAddressPair& match);

  // Getter/setter for the BinExport filename
  void set_filename(absl::string_view filename) {
    filename_.assign(filename.data(), filename.size());
  }
  const std::string& filename() const { return filename_; }

  // Getter/setter for the executable hash of the binary represented by this
  // column.
  void set_sha256(absl::string_view hash) {
    sha256_.assign(hash.data(), hash.size());
  }
  const std::string& sha256() const { return sha256_; }

  // Getter/setter for the BinDiff result directory
  void set_diff_directory(absl::string_view directory) {
    diff_directory_.assign(directory.data(), directory.size());
  }
  const std::string& diff_directory() const { return diff_directory_; }

  // Getter/setter for the function filter mode
  void set_function_filter(SignatureDefinition::FunctionFilterMode value) {
    function_filter_ = value;
  }
  SignatureDefinition::FunctionFilterMode function_filter() {
    return function_filter_;
  }

  // Add a function address to the list of filtered functions. Filtering
  // behavior is controlled by calling set_function_filter().
  void AddFilteredFunction(MemoryAddress address) {
    filtered_functions_.insert(address);
  }

  // Accessor functions to return function, basic block and instruction indices.
  const FunctionAddressIndex& functions_by_address() const {
    return functions_by_address_;
  }
  static FunctionAddressIndex* GetFunctionIndexFromColumn(
      MatchChainColumn* column) {
    return &column->functions_by_address_;
  }
  static BasicBlockAddressIndex* GetBasicBlockIndexFromColumn(
      MatchChainColumn* column) {
    return &column->basic_blocks_by_address_;
  }

  // Lookup functions to find functions, basic blocks and instructions by
  // address.
  MatchedFunction* FindFunctionByAddress(MemoryAddress address);
  MatchedBasicBlock* FindBasicBlockByAddress(MemoryAddress address);
  MatchedInstruction* FindInstructionByAddress(MemoryAddress address);

  // Lookup functions to find functions and basic blocks by id. Note that we
  // don't need a lookup function for instructions by id, since we're already
  // operating at instruction byte-level at that point.
  MatchedFunction* FindFunctionById(Ident id);
  MatchedBasicBlock* FindBasicBlockById(Ident id);

  // Terminate the match chain table by propagating the next to last column's
  // address_in_next to this column's address and adding mappings to address
  // zero. This is done because we have one more binary than BinDiff results.
  void FinishChain(MatchChainColumn* prev);

  // Build id indices for functions and basic blocks to support the Find*ById
  // family of functions. Should be called after all functions and basic blocks
  // have been added to this column.
  void BuildIdIndices();

 private:
  // If set to FILTER_BLACKLIST, filtered_functions_ will serve as a function
  // blacklist. Set to FILTER_WHITELIST, _only_ the functions listed in
  // filtered_functions_ will be used.
  SignatureDefinition::FunctionFilterMode function_filter_ =
      SignatureDefinition::FILTER_NONE;
  absl::flat_hash_set<MemoryAddress> filtered_functions_;

  // These map memory addresses to function, basic block and instruction
  // matches. The members below also own the matched objects.
  FunctionAddressIndex functions_by_address_;
  BasicBlockAddressIndex basic_blocks_by_address_;
  InstructionAddressIndex instructions_by_address_;

  // Indices that get build on demand for calculating candidates.
  FunctionIdentIndex functions_by_id_;
  BasicBlockIdentIndex basic_blocks_by_id_;

  std::string filename_;
  std::string sha256_;
  std::string diff_directory_;
};

// Multiple MatchChainColumns make up the match chain table.
using MatchChainTable = std::vector<std::unique_ptr<MatchChainColumn>>;

// Adds a diff result file to the table in the specified column.
not_absl::Status AddDiffResult(
    absl::string_view filename, bool last, MatchChainColumn* column,
    MatchChainColumn* next,
    std::vector<std::pair<std::string, std::string>>* diffs);

// Loads function metadata and raw instruction bytes from the specified
// .BinExport file and adds it to the table in the specified column.
not_absl::Status AddFunctionData(absl::string_view filename,
                                 MatchChainColumn* column);

// Imposes an order on the matches of each column/binary in the table. The
// first column is used as the "master column", i.e. the matches of the
// first column are simply enumerated by their ascending addresses and their
// respective position is stored in the id member. This function then tries
// to build chains for each id so that the ids of each column are
// permutations of the ids of the first column.
void PropagateIds(MatchChainTable* table);

// Builds id indices for all columns of the specified MatchChainTable by
// calling the method of the same name on its columns.
void BuildIdIndices(MatchChainTable* table);

}  // namespace vxsig
}  // namespace security

#endif  // VXSIG_MATCH_CHAIN_TABLE_H_
