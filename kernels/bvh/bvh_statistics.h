// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "bvh.h"
#include <sstream>

namespace embree
{
  template<int N>
  class BVHNStatistics
  {
    typedef BVHN<N> BVH;
    typedef typename BVH::AlignedNode AlignedNode;
    typedef typename BVH::UnalignedNode UnalignedNode;
    typedef typename BVH::AlignedNodeMB AlignedNodeMB;
    typedef typename BVH::UnalignedNodeMB UnalignedNodeMB;
    typedef typename BVH::TransformNode TransformNode;
    typedef typename BVH::QuantizedNode QuantizedNode;

    typedef typename BVH::NodeRef NodeRef;

    struct Statistics 
    {
      template<typename Node>
        struct NodeStat
      {
        NodeStat ( double nodeSAH = 0,
                   size_t numNodes = 0, 
                   size_t numChildren = 0)
        : nodeSAH(nodeSAH),
          numNodes(numNodes), 
          numChildren(numChildren) {}
        
        double sah(BVH* bvh) const {
          return nodeSAH/halfArea(bvh->getBounds());
        }

        size_t bytes() const {
          return numNodes*sizeof(Node);
        }

        size_t size() const {
          return numNodes;
        }

        double fillRateNom () const { return double(numChildren);  }
        double fillRateDen () const { return numNodes*N;  }
        double fillRate    () const { return fillRateNom()/fillRateDen(); }

        __forceinline friend NodeStat operator+ ( const NodeStat& a, const NodeStat& b)
        {
          return NodeStat(a.nodeSAH + b.nodeSAH,
                          a.numNodes+b.numNodes,
                          a.numChildren+b.numChildren);
        }

        std::string toString(BVH* bvh, double sahTotal, size_t bytesTotal) const
        {
          std::ostringstream stream;
          stream.setf(std::ios::fixed, std::ios::floatfield);
          stream << "sah = " << std::setw(7) << std::setprecision(3) << sah(bvh);
          stream << " (" << std::setw(6) << std::setprecision(2) << 100.0*sah(bvh)/sahTotal << "%), ";          
          stream << "#bytes = " << std::setw(7) << std::setprecision(2) << bytes()/1E6  << " MB ";
          stream << "(" << std::setw(6) << std::setprecision(2) << 100.0*double(bytes())/double(bytesTotal) << "%), ";
          stream << "#nodes = " << std::setw(7) << numNodes << " (" << std::setw(6) << std::setprecision(2) << 100.0*fillRate() << "% filled)";
          return stream.str();
        }

      public:
        double nodeSAH;
        size_t numNodes;
        size_t numChildren;
      };

      struct LeafStat
      {
        LeafStat ( double leafSAH = 0.0f, 
                   size_t numLeaves = 0,
                   size_t numPrims = 0,
                   size_t numPrimBlocks = 0 )
        : leafSAH(leafSAH),
          numLeaves(numLeaves),
          numPrims(numPrims),
          numPrimBlocks(numPrimBlocks) {}

        double sah(BVH* bvh) const {
          return leafSAH/halfArea(bvh->getBounds());
        }

        size_t bytes(BVH* bvh) const {
          return numPrimBlocks*bvh->primTy.bytes;
        }

        size_t size() const {
          return numLeaves;
        }

        double fillRateNom (BVH* bvh) const { return double(numPrims);  }
        double fillRateDen (BVH* bvh) const { return double(bvh->primTy.blockSize*numPrimBlocks);  }
        double fillRate    (BVH* bvh) const { return fillRateNom(bvh)/fillRateDen(bvh); }

        __forceinline friend LeafStat operator+ ( const LeafStat& a, const LeafStat& b)
        {
          return LeafStat(a.leafSAH + b.leafSAH,
                          a.numLeaves+b.numLeaves,
                          a.numPrims+b.numPrims,
                          a.numPrimBlocks+b.numPrimBlocks);
        }

        std::string toString(BVH* bvh, double sahTotal, size_t bytesTotal) const
        {
          std::ostringstream stream;
          stream.setf(std::ios::fixed, std::ios::floatfield);
          stream << "sah = " << std::setw(7) << std::setprecision(3) << sah(bvh);
          stream << " (" << std::setw(6) << std::setprecision(2) << 100.0*sah(bvh)/sahTotal << "%), ";
          stream << "#bytes = " << std::setw(7) << std::setprecision(2) << double(bytes(bvh))/1E6  << " MB ";
          stream << "(" << std::setw(6) << std::setprecision(2) << 100.0*double(bytes(bvh))/double(bytesTotal) << "%), ";
          stream << "#nodes = " << std::setw(7) << numLeaves << " (" << std::setw(6) << std::setprecision(2) << 100.0*fillRate(bvh) << "% filled)";
          return stream.str();
        }
     
      public:
        double leafSAH;                    //!< SAH of the leaves only
        size_t numLeaves;                  //!< Number of leaf nodes.
        size_t numPrims;                   //!< Number of primitives.
        size_t numPrimBlocks;              //!< Number of primitive blocks.
      };

    public:
      Statistics (size_t depth = 0,
                  LeafStat statLeaf = LeafStat(),
                  NodeStat<AlignedNode> statAlignedNodes = NodeStat<AlignedNode>(),
                  NodeStat<UnalignedNode> statUnalignedNodes = NodeStat<UnalignedNode>(),
                  NodeStat<AlignedNodeMB> statAlignedNodesMB = NodeStat<AlignedNodeMB>(),
                  NodeStat<UnalignedNodeMB> statUnalignedNodesMB = NodeStat<UnalignedNodeMB>(),
                  NodeStat<TransformNode> statTransformNodes = NodeStat<TransformNode>(),
                  NodeStat<QuantizedNode> statQuantizedNodes = NodeStat<QuantizedNode>())

      : depth(depth), 
        statLeaf(statLeaf),
        statAlignedNodes(statAlignedNodes),
        statUnalignedNodes(statUnalignedNodes),
        statAlignedNodesMB(statAlignedNodesMB),
        statUnalignedNodesMB(statUnalignedNodesMB),
        statTransformNodes(statTransformNodes),
        statQuantizedNodes(statQuantizedNodes) {}

      double sah(BVH* bvh) const 
      {
        return statLeaf.sah(bvh) +
          statAlignedNodes.sah(bvh) + 
          statUnalignedNodes.sah(bvh) + 
          statAlignedNodesMB.sah(bvh) + 
          statUnalignedNodesMB.sah(bvh) + 
          statTransformNodes.sah(bvh) + 
          statQuantizedNodes.sah(bvh);
      }
      
      size_t bytes(BVH* bvh) const {
        return statLeaf.bytes(bvh) +
          statAlignedNodes.bytes() + 
          statUnalignedNodes.bytes() + 
          statAlignedNodesMB.bytes() + 
          statUnalignedNodesMB.bytes() + 
          statTransformNodes.bytes() + 
          statQuantizedNodes.bytes();
      }

      size_t size() const 
      {
        return statLeaf.size() +
          statAlignedNodes.size() + 
          statUnalignedNodes.size() + 
          statAlignedNodesMB.size() + 
          statUnalignedNodesMB.size() + 
          statTransformNodes.size() + 
          statQuantizedNodes.size();
      }

      double fillRate (BVH* bvh) const 
      {
        double nom = statLeaf.fillRateNom(bvh) +
          statAlignedNodes.fillRateNom() + 
          statUnalignedNodes.fillRateNom() + 
          statAlignedNodesMB.fillRateNom() + 
          statUnalignedNodesMB.fillRateNom() + 
          statTransformNodes.fillRateNom() + 
          statQuantizedNodes.fillRateNom();
        double den = statLeaf.fillRateDen(bvh) +
          statAlignedNodes.fillRateDen() + 
          statUnalignedNodes.fillRateDen() + 
          statAlignedNodesMB.fillRateDen() + 
          statUnalignedNodesMB.fillRateDen() + 
          statTransformNodes.fillRateDen() + 
          statQuantizedNodes.fillRateDen();
        return nom/den;
      }

      friend Statistics operator+ ( const Statistics& a, const Statistics& b )
      {
        return Statistics(max(a.depth,b.depth),
                          a.statLeaf + b.statLeaf,
                          a.statAlignedNodes + b.statAlignedNodes,
                          a.statUnalignedNodes + b.statUnalignedNodes,
                          a.statAlignedNodesMB + b.statAlignedNodesMB,
                          a.statUnalignedNodesMB + b.statUnalignedNodesMB,
                          a.statTransformNodes + b.statTransformNodes,
                          a.statQuantizedNodes + b.statQuantizedNodes);
      }

    public:
      size_t depth;
      LeafStat statLeaf;
      NodeStat<AlignedNode> statAlignedNodes;
      NodeStat<UnalignedNode> statUnalignedNodes;
      NodeStat<AlignedNodeMB> statAlignedNodesMB;
      NodeStat<UnalignedNodeMB> statUnalignedNodesMB;
      NodeStat<TransformNode> statTransformNodes;
      NodeStat<QuantizedNode> statQuantizedNodes;
    };

  public:

    /* Constructor gathers statistics. */
    BVHNStatistics (BVH* bvh);

    /*! Convert statistics into a string */
    std::string str();

    double sah() const { 
      return stat.sah(bvh); 
    }

    size_t bytesUsed() const {
      return stat.bytes(bvh);
    }

  private:
    Statistics statistics(NodeRef node, const double A, const BBox1f dt);

  private:
    BVH* bvh;
    Statistics stat;
  };

  typedef BVHNStatistics<4> BVH4Statistics;
  typedef BVHNStatistics<8> BVH8Statistics;
}
