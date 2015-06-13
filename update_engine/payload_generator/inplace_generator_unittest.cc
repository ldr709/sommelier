// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/payload_generator/inplace_generator.h"

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <base/logging.h>
#include <base/strings/string_util.h>
#include <gtest/gtest.h>

#include "update_engine/payload_generator/cycle_breaker.h"
#include "update_engine/payload_generator/delta_diff_generator.h"
#include "update_engine/payload_generator/extent_ranges.h"
#include "update_engine/payload_generator/graph_types.h"
#include "update_engine/payload_generator/graph_utils.h"
#include "update_engine/test_utils.h"
#include "update_engine/utils.h"

using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

namespace chromeos_update_engine {

using Block = InplaceGenerator::Block;

namespace {

#define OP_BSDIFF DeltaArchiveManifest_InstallOperation_Type_BSDIFF
#define OP_MOVE DeltaArchiveManifest_InstallOperation_Type_MOVE
#define OP_REPLACE DeltaArchiveManifest_InstallOperation_Type_REPLACE
#define OP_REPLACE_BZ DeltaArchiveManifest_InstallOperation_Type_REPLACE_BZ

void GenVertex(Vertex* out,
               const vector<Extent>& src_extents,
               const vector<Extent>& dst_extents,
               const string& path,
               DeltaArchiveManifest_InstallOperation_Type type) {
  out->op.set_type(type);
  out->file_name = path;
  StoreExtents(src_extents, out->op.mutable_src_extents());
  StoreExtents(dst_extents, out->op.mutable_dst_extents());
}

vector<Extent> VectOfExt(uint64_t start_block, uint64_t num_blocks) {
  return vector<Extent>(1, ExtentForRange(start_block, num_blocks));
}

EdgeProperties EdgeWithReadDep(const vector<Extent>& extents) {
  EdgeProperties ret;
  ret.extents = extents;
  return ret;
}

EdgeProperties EdgeWithWriteDep(const vector<Extent>& extents) {
  EdgeProperties ret;
  ret.write_extents = extents;
  return ret;
}

template<typename T>
void DumpVect(const vector<T>& vect) {
  stringstream ss(stringstream::out);
  for (typename vector<T>::const_iterator it = vect.begin(), e = vect.end();
       it != e; ++it) {
    ss << *it << ", ";
  }
  LOG(INFO) << "{" << ss.str() << "}";
}

void AppendExtent(vector<Extent>* vect, uint64_t start, uint64_t length) {
  vect->resize(vect->size() + 1);
  vect->back().set_start_block(start);
  vect->back().set_num_blocks(length);
}

void OpAppendExtent(DeltaArchiveManifest_InstallOperation* op,
                    uint64_t start,
                    uint64_t length) {
  Extent* extent = op->add_src_extents();
  extent->set_start_block(start);
  extent->set_num_blocks(length);
}

}  // namespace

class InplaceGeneratorTest : public ::testing::Test {
};

TEST_F(InplaceGeneratorTest, BlockDefaultValues) {
  // Tests that a Block is initialized with the default values as a
  // Vertex::kInvalidIndex. This is required by the delta generators.
  Block block;
  EXPECT_EQ(Vertex::kInvalidIndex, block.reader);
  EXPECT_EQ(Vertex::kInvalidIndex, block.writer);
}

TEST_F(InplaceGeneratorTest, SubstituteBlocksTest) {
  vector<Extent> remove_blocks;
  AppendExtent(&remove_blocks, 3, 3);
  AppendExtent(&remove_blocks, 7, 1);
  vector<Extent> replace_blocks;
  AppendExtent(&replace_blocks, 10, 2);
  AppendExtent(&replace_blocks, 13, 2);
  Vertex vertex;
  DeltaArchiveManifest_InstallOperation& op = vertex.op;
  OpAppendExtent(&op, 4, 3);
  OpAppendExtent(&op, kSparseHole, 4);  // Sparse hole in file
  OpAppendExtent(&op, 3, 1);
  OpAppendExtent(&op, 7, 3);

  InplaceGenerator::SubstituteBlocks(&vertex, remove_blocks, replace_blocks);

  EXPECT_EQ(7, op.src_extents_size());
  EXPECT_EQ(11, op.src_extents(0).start_block());
  EXPECT_EQ(1, op.src_extents(0).num_blocks());
  EXPECT_EQ(13, op.src_extents(1).start_block());
  EXPECT_EQ(1, op.src_extents(1).num_blocks());
  EXPECT_EQ(6, op.src_extents(2).start_block());
  EXPECT_EQ(1, op.src_extents(2).num_blocks());
  EXPECT_EQ(kSparseHole, op.src_extents(3).start_block());
  EXPECT_EQ(4, op.src_extents(3).num_blocks());
  EXPECT_EQ(10, op.src_extents(4).start_block());
  EXPECT_EQ(1, op.src_extents(4).num_blocks());
  EXPECT_EQ(14, op.src_extents(5).start_block());
  EXPECT_EQ(1, op.src_extents(5).num_blocks());
  EXPECT_EQ(8, op.src_extents(6).start_block());
  EXPECT_EQ(2, op.src_extents(6).num_blocks());
}

TEST_F(InplaceGeneratorTest, CutEdgesTest) {
  Graph graph;
  vector<Block> blocks(9);

  // Create nodes in graph
  {
    graph.resize(graph.size() + 1);
    graph.back().op.set_type(DeltaArchiveManifest_InstallOperation_Type_MOVE);
    // Reads from blocks 3, 5, 7
    vector<Extent> extents;
    AppendBlockToExtents(&extents, 3);
    AppendBlockToExtents(&extents, 5);
    AppendBlockToExtents(&extents, 7);
    StoreExtents(extents,
                                     graph.back().op.mutable_src_extents());
    blocks[3].reader = graph.size() - 1;
    blocks[5].reader = graph.size() - 1;
    blocks[7].reader = graph.size() - 1;

    // Writes to blocks 1, 2, 4
    extents.clear();
    AppendBlockToExtents(&extents, 1);
    AppendBlockToExtents(&extents, 2);
    AppendBlockToExtents(&extents, 4);
    StoreExtents(extents,
                                     graph.back().op.mutable_dst_extents());
    blocks[1].writer = graph.size() - 1;
    blocks[2].writer = graph.size() - 1;
    blocks[4].writer = graph.size() - 1;
  }
  {
    graph.resize(graph.size() + 1);
    graph.back().op.set_type(DeltaArchiveManifest_InstallOperation_Type_MOVE);
    // Reads from blocks 1, 2, 4
    vector<Extent> extents;
    AppendBlockToExtents(&extents, 1);
    AppendBlockToExtents(&extents, 2);
    AppendBlockToExtents(&extents, 4);
    StoreExtents(extents,
                                     graph.back().op.mutable_src_extents());
    blocks[1].reader = graph.size() - 1;
    blocks[2].reader = graph.size() - 1;
    blocks[4].reader = graph.size() - 1;

    // Writes to blocks 3, 5, 6
    extents.clear();
    AppendBlockToExtents(&extents, 3);
    AppendBlockToExtents(&extents, 5);
    AppendBlockToExtents(&extents, 6);
    StoreExtents(extents,
                                     graph.back().op.mutable_dst_extents());
    blocks[3].writer = graph.size() - 1;
    blocks[5].writer = graph.size() - 1;
    blocks[6].writer = graph.size() - 1;
  }

  // Create edges
  InplaceGenerator::CreateEdges(&graph, blocks);

  // Find cycles
  CycleBreaker cycle_breaker;
  set<Edge> cut_edges;
  cycle_breaker.BreakCycles(graph, &cut_edges);

  EXPECT_EQ(1, cut_edges.size());
  EXPECT_TRUE(cut_edges.end() != cut_edges.find(
      std::pair<Vertex::Index, Vertex::Index>(1, 0)));

  vector<CutEdgeVertexes> cuts;
  EXPECT_TRUE(InplaceGenerator::CutEdges(&graph, cut_edges, &cuts));

  EXPECT_EQ(3, graph.size());

  // Check new node in graph:
  EXPECT_EQ(DeltaArchiveManifest_InstallOperation_Type_MOVE,
            graph.back().op.type());
  EXPECT_EQ(2, graph.back().op.src_extents_size());
  EXPECT_EQ(1, graph.back().op.dst_extents_size());
  EXPECT_EQ(kTempBlockStart, graph.back().op.dst_extents(0).start_block());
  EXPECT_EQ(2, graph.back().op.dst_extents(0).num_blocks());
  EXPECT_TRUE(graph.back().out_edges.empty());

  // Check that old node reads from new blocks
  EXPECT_EQ(2, graph[0].op.src_extents_size());
  EXPECT_EQ(kTempBlockStart, graph[0].op.src_extents(0).start_block());
  EXPECT_EQ(2, graph[0].op.src_extents(0).num_blocks());
  EXPECT_EQ(7, graph[0].op.src_extents(1).start_block());
  EXPECT_EQ(1, graph[0].op.src_extents(1).num_blocks());

  // And that the old dst extents haven't changed
  EXPECT_EQ(2, graph[0].op.dst_extents_size());
  EXPECT_EQ(1, graph[0].op.dst_extents(0).start_block());
  EXPECT_EQ(2, graph[0].op.dst_extents(0).num_blocks());
  EXPECT_EQ(4, graph[0].op.dst_extents(1).start_block());
  EXPECT_EQ(1, graph[0].op.dst_extents(1).num_blocks());

  // Ensure it only depends on the next node and the new temp node
  EXPECT_EQ(2, graph[0].out_edges.size());
  EXPECT_TRUE(graph[0].out_edges.end() != graph[0].out_edges.find(1));
  EXPECT_TRUE(graph[0].out_edges.end() != graph[0].out_edges.find(graph.size() -
                                                                  1));

  // Check second node has unchanged extents
  EXPECT_EQ(2, graph[1].op.src_extents_size());
  EXPECT_EQ(1, graph[1].op.src_extents(0).start_block());
  EXPECT_EQ(2, graph[1].op.src_extents(0).num_blocks());
  EXPECT_EQ(4, graph[1].op.src_extents(1).start_block());
  EXPECT_EQ(1, graph[1].op.src_extents(1).num_blocks());

  EXPECT_EQ(2, graph[1].op.dst_extents_size());
  EXPECT_EQ(3, graph[1].op.dst_extents(0).start_block());
  EXPECT_EQ(1, graph[1].op.dst_extents(0).num_blocks());
  EXPECT_EQ(5, graph[1].op.dst_extents(1).start_block());
  EXPECT_EQ(2, graph[1].op.dst_extents(1).num_blocks());

  // Ensure it only depends on the next node
  EXPECT_EQ(1, graph[1].out_edges.size());
  EXPECT_TRUE(graph[1].out_edges.end() != graph[1].out_edges.find(2));
}

TEST_F(InplaceGeneratorTest, AssignTempBlocksReuseTest) {
  Graph graph(9);

  const vector<Extent> empt;
  uint64_t tmp = kTempBlockStart;
  const string kFilename = "/foo";

  vector<CutEdgeVertexes> cuts;
  cuts.resize(3);

  // Simple broken loop:
  GenVertex(&graph[0], VectOfExt(0, 1), VectOfExt(1, 1), "", OP_MOVE);
  GenVertex(&graph[1], VectOfExt(tmp, 1), VectOfExt(0, 1), "", OP_MOVE);
  GenVertex(&graph[2], VectOfExt(1, 1), VectOfExt(tmp, 1), "", OP_MOVE);
  // Corresponding edges:
  graph[0].out_edges[2] = EdgeWithReadDep(VectOfExt(1, 1));
  graph[1].out_edges[2] = EdgeWithWriteDep(VectOfExt(tmp, 1));
  graph[1].out_edges[0] = EdgeWithReadDep(VectOfExt(0, 1));
  // Store the cut:
  cuts[0].old_dst = 1;
  cuts[0].old_src = 0;
  cuts[0].new_vertex = 2;
  cuts[0].tmp_extents = VectOfExt(tmp, 1);
  tmp++;

  // Slightly more complex pair of loops:
  GenVertex(&graph[3], VectOfExt(4, 2), VectOfExt(2, 2), "", OP_MOVE);
  GenVertex(&graph[4], VectOfExt(6, 1), VectOfExt(7, 1), "", OP_MOVE);
  GenVertex(&graph[5], VectOfExt(tmp, 3), VectOfExt(4, 3), kFilename, OP_MOVE);
  GenVertex(&graph[6], VectOfExt(2, 2), VectOfExt(tmp, 2), "", OP_MOVE);
  GenVertex(&graph[7], VectOfExt(7, 1), VectOfExt(tmp + 2, 1), "", OP_MOVE);
  // Corresponding edges:
  graph[3].out_edges[6] = EdgeWithReadDep(VectOfExt(2, 2));
  graph[4].out_edges[7] = EdgeWithReadDep(VectOfExt(7, 1));
  graph[5].out_edges[6] = EdgeWithWriteDep(VectOfExt(tmp, 2));
  graph[5].out_edges[7] = EdgeWithWriteDep(VectOfExt(tmp + 2, 1));
  graph[5].out_edges[3] = EdgeWithReadDep(VectOfExt(4, 2));
  graph[5].out_edges[4] = EdgeWithReadDep(VectOfExt(6, 1));
  // Store the cuts:
  cuts[1].old_dst = 5;
  cuts[1].old_src = 3;
  cuts[1].new_vertex = 6;
  cuts[1].tmp_extents = VectOfExt(tmp, 2);
  cuts[2].old_dst = 5;
  cuts[2].old_src = 4;
  cuts[2].new_vertex = 7;
  cuts[2].tmp_extents = VectOfExt(tmp + 2, 1);

  // Supplier of temp block:
  GenVertex(&graph[8], empt, VectOfExt(8, 1), "", OP_REPLACE);

  // Specify the final order:
  vector<Vertex::Index> op_indexes;
  op_indexes.push_back(2);
  op_indexes.push_back(0);
  op_indexes.push_back(1);
  op_indexes.push_back(6);
  op_indexes.push_back(3);
  op_indexes.push_back(7);
  op_indexes.push_back(4);
  op_indexes.push_back(5);
  op_indexes.push_back(8);

  vector<vector<Vertex::Index>::size_type> reverse_op_indexes;
  InplaceGenerator::GenerateReverseTopoOrderMap(op_indexes,
                                                &reverse_op_indexes);

  int fd;
  EXPECT_TRUE(utils::MakeTempFile("AssignTempBlocksReuseTest.XXXXXX",
                                  nullptr,
                                  &fd));
  ScopedFdCloser fd_closer(&fd);
  off_t data_file_size = 0;

  EXPECT_TRUE(InplaceGenerator::AssignTempBlocks(&graph,
                                                 "/dev/zero",
                                                 fd,
                                                 &data_file_size,
                                                 &op_indexes,
                                                 &reverse_op_indexes,
                                                 cuts));
  EXPECT_FALSE(graph[6].valid);
  EXPECT_FALSE(graph[7].valid);
  EXPECT_EQ(1, graph[1].op.src_extents_size());
  EXPECT_EQ(2, graph[1].op.src_extents(0).start_block());
  EXPECT_EQ(1, graph[1].op.src_extents(0).num_blocks());
  EXPECT_EQ(OP_REPLACE_BZ, graph[5].op.type());
}

TEST_F(InplaceGeneratorTest, MoveFullOpsToBackTest) {
  Graph graph(4);
  graph[0].file_name = "A";
  graph[0].op.set_type(DeltaArchiveManifest_InstallOperation_Type_REPLACE);
  graph[1].file_name = "B";
  graph[1].op.set_type(DeltaArchiveManifest_InstallOperation_Type_BSDIFF);
  graph[2].file_name = "C";
  graph[2].op.set_type(DeltaArchiveManifest_InstallOperation_Type_REPLACE_BZ);
  graph[3].file_name = "D";
  graph[3].op.set_type(DeltaArchiveManifest_InstallOperation_Type_MOVE);

  vector<Vertex::Index> vect(graph.size());

  for (vector<Vertex::Index>::size_type i = 0; i < vect.size(); ++i) {
    vect[i] = i;
  }
  InplaceGenerator::MoveFullOpsToBack(&graph, &vect);
  EXPECT_EQ(vect.size(), graph.size());
  EXPECT_EQ(graph[vect[0]].file_name, "B");
  EXPECT_EQ(graph[vect[1]].file_name, "D");
  EXPECT_EQ(graph[vect[2]].file_name, "A");
  EXPECT_EQ(graph[vect[3]].file_name, "C");
}

TEST_F(InplaceGeneratorTest, AssignTempBlocksTest) {
  Graph graph(9);
  const vector<Extent> empt;  // empty
  const string kFilename = "/foo";

  // Some scratch space:
  GenVertex(&graph[0], empt, VectOfExt(200, 1), "", OP_REPLACE);
  GenVertex(&graph[1], empt, VectOfExt(210, 10), "", OP_REPLACE);
  GenVertex(&graph[2], empt, VectOfExt(220, 1), "", OP_REPLACE);

  // A cycle that requires 10 blocks to break:
  GenVertex(&graph[3], VectOfExt(10, 11), VectOfExt(0, 9), "", OP_BSDIFF);
  graph[3].out_edges[4] = EdgeWithReadDep(VectOfExt(0, 9));
  GenVertex(&graph[4], VectOfExt(0, 9), VectOfExt(10, 11), "", OP_BSDIFF);
  graph[4].out_edges[3] = EdgeWithReadDep(VectOfExt(10, 11));

  // A cycle that requires 9 blocks to break:
  GenVertex(&graph[5], VectOfExt(40, 11), VectOfExt(30, 10), "", OP_BSDIFF);
  graph[5].out_edges[6] = EdgeWithReadDep(VectOfExt(30, 10));
  GenVertex(&graph[6], VectOfExt(30, 10), VectOfExt(40, 11), "", OP_BSDIFF);
  graph[6].out_edges[5] = EdgeWithReadDep(VectOfExt(40, 11));

  // A cycle that requires 40 blocks to break (which is too many):
  GenVertex(&graph[7],
            VectOfExt(120, 50),
            VectOfExt(60, 40),
            "",
            OP_BSDIFF);
  graph[7].out_edges[8] = EdgeWithReadDep(VectOfExt(60, 40));
  GenVertex(&graph[8],
            VectOfExt(60, 40),
            VectOfExt(120, 50),
            kFilename,
            OP_BSDIFF);
  graph[8].out_edges[7] = EdgeWithReadDep(VectOfExt(120, 50));

  graph_utils::DumpGraph(graph);

  vector<Vertex::Index> final_order;

  int fd;
  EXPECT_TRUE(utils::MakeTempFile("AssignTempBlocksTestData.XXXXXX",
                                  nullptr,
                                  &fd));
  ScopedFdCloser fd_closer(&fd);
  off_t data_file_size = 0;

  EXPECT_TRUE(InplaceGenerator::ConvertGraphToDag(&graph,
                                                  "/dev/zero",
                                                  fd,
                                                  &data_file_size,
                                                  &final_order,
                                                  Vertex::kInvalidIndex));

  Graph expected_graph(12);
  GenVertex(&expected_graph[0], empt, VectOfExt(200, 1), "", OP_REPLACE);
  GenVertex(&expected_graph[1], empt, VectOfExt(210, 10), "", OP_REPLACE);
  GenVertex(&expected_graph[2], empt, VectOfExt(220, 1), "", OP_REPLACE);
  GenVertex(&expected_graph[3],
            VectOfExt(10, 11),
            VectOfExt(0, 9),
            "",
            OP_BSDIFF);
  expected_graph[3].out_edges[9] = EdgeWithReadDep(VectOfExt(0, 9));
  GenVertex(&expected_graph[4],
            VectOfExt(60, 9),
            VectOfExt(10, 11),
            "",
            OP_BSDIFF);
  expected_graph[4].out_edges[3] = EdgeWithReadDep(VectOfExt(10, 11));
  expected_graph[4].out_edges[9] = EdgeWithWriteDep(VectOfExt(60, 9));
  GenVertex(&expected_graph[5],
            VectOfExt(40, 11),
            VectOfExt(30, 10),
            "",
            OP_BSDIFF);
  expected_graph[5].out_edges[10] = EdgeWithReadDep(VectOfExt(30, 10));

  GenVertex(&expected_graph[6],
            VectOfExt(60, 10),
            VectOfExt(40, 11),
            "",
            OP_BSDIFF);
  expected_graph[6].out_edges[5] = EdgeWithReadDep(VectOfExt(40, 11));
  expected_graph[6].out_edges[10] = EdgeWithWriteDep(VectOfExt(60, 10));

  GenVertex(&expected_graph[7],
            VectOfExt(120, 50),
            VectOfExt(60, 40),
            "",
            OP_BSDIFF);
  expected_graph[7].out_edges[6] = EdgeWithReadDep(VectOfExt(60, 10));

  GenVertex(&expected_graph[8], empt, VectOfExt(0, 50), "/foo", OP_REPLACE_BZ);
  expected_graph[8].out_edges[7] = EdgeWithReadDep(VectOfExt(120, 50));

  GenVertex(&expected_graph[9],
            VectOfExt(0, 9),
            VectOfExt(60, 9),
            "",
            OP_MOVE);

  GenVertex(&expected_graph[10],
            VectOfExt(30, 10),
            VectOfExt(60, 10),
            "",
            OP_MOVE);
  expected_graph[10].out_edges[4] = EdgeWithReadDep(VectOfExt(60, 9));

  EXPECT_EQ(12, graph.size());
  EXPECT_FALSE(graph.back().valid);
  for (Graph::size_type i = 0; i < graph.size() - 1; i++) {
    EXPECT_TRUE(graph[i].out_edges == expected_graph[i].out_edges);
    if (i == 8) {
      // special case
    } else {
      // EXPECT_TRUE(graph[i] == expected_graph[i]) << "i = " << i;
    }
  }
}

TEST_F(InplaceGeneratorTest, CreateScratchNodeTest) {
  Vertex vertex;
  InplaceGenerator::CreateScratchNode(12, 34, &vertex);
  EXPECT_EQ(DeltaArchiveManifest_InstallOperation_Type_REPLACE_BZ,
            vertex.op.type());
  EXPECT_EQ(0, vertex.op.data_offset());
  EXPECT_EQ(0, vertex.op.data_length());
  EXPECT_EQ(1, vertex.op.dst_extents_size());
  EXPECT_EQ(12, vertex.op.dst_extents(0).start_block());
  EXPECT_EQ(34, vertex.op.dst_extents(0).num_blocks());
}

TEST_F(InplaceGeneratorTest, ApplyMapTest) {
  vector<uint64_t> collection = {1, 2, 3, 4, 6};
  vector<uint64_t> expected_values = {1, 2, 5, 4, 8};
  map<uint64_t, uint64_t> value_map;
  value_map[3] = 5;
  value_map[6] = 8;
  value_map[5] = 10;

  InplaceGenerator::ApplyMap(&collection, value_map);
  EXPECT_EQ(expected_values, collection);
}

}  // namespace chromeos_update_engine
