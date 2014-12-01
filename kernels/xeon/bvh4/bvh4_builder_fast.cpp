// ======================================================================== //
// Copyright 2009-2014 Intel Corporation                                    //
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

#include "bvh4.h"
#include "bvh4_builder_fast.h"
#include "bvh4_statistics.h"
#include "builders/primrefgen.h"

#include "geometry/bezier1v.h"
#include "geometry/bezier1i.h"
#include "geometry/triangle1.h"
#include "geometry/triangle4.h"
#include "geometry/triangle8.h"
#include "geometry/triangle1v.h"
#include "geometry/triangle4v.h"
#include "geometry/triangle4i.h"
#include "geometry/subdivpatch1.h"
#include "geometry/subdivpatch1cached.h"

//#include "geometry/subdivpatchdispl1.h"
//#include "geometry/quadquad4x4_subdiv.h"

#include "geometry/quadquad4x4.h"
#include "common/subdiv/feature_adaptive_gregory.h"
#include "common/subdiv/feature_adaptive_bspline.h"
#include "geometry/virtual_accel.h"

#include <algorithm>

#define DBG(x) 

//#define PROFILE
#define SUBDIVISION_LEVEL_DISPL 9 // FIXME: remove

namespace embree
{
  namespace isa
  {
    static double dt = 0.0f;

    static const size_t THRESHOLD_FOR_SUBTREE_RECURSION = 128;
    static const size_t THRESHOLD_FOR_SINGLE_THREADED = 50000; // FIXME: measure if this is really optimal, maybe disable only parallel splits

    BVH4BuilderFast::BVH4BuilderFast (LockStepTaskScheduler* scheduler, BVH4* bvh, size_t listMode, size_t logBlockSize, size_t logSAHBlockSize, 
				      bool needVertices, size_t primBytes, const size_t minLeafSize, const size_t maxLeafSize)
      : scheduler(scheduler), state(nullptr), bvh(bvh), numPrimitives(0), prims(NULL), bytesPrims(0), listMode(listMode), logBlockSize(logBlockSize), logSAHBlockSize(logSAHBlockSize), 
	needVertices(needVertices), primBytes(primBytes), minLeafSize(minLeafSize), maxLeafSize(maxLeafSize) { needAllThreads = true; }

    template<typename Primitive>
    BVH4BuilderFastT<Primitive>::BVH4BuilderFastT (BVH4* bvh, Scene* scene, size_t listMode, size_t logBlockSize, size_t logSAHBlockSize, 
						   bool needVertices, size_t primBytes, const size_t minLeafSize, const size_t maxLeafSize,bool parallel)
      : scene(scene), BVH4BuilderFast(&scene->lockstep_scheduler,bvh,listMode,logBlockSize,logSAHBlockSize,needVertices,primBytes,minLeafSize,maxLeafSize) { needAllThreads = parallel; }
    
    template<> BVH4BezierBuilderFast  <Bezier1v>   ::BVH4BezierBuilderFast   (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Bezier1v>   (bvh,scene,listMode,0,0,false,sizeof(Bezier1v),1,1,true) {}
    template<> BVH4BezierBuilderFast  <Bezier1i>  ::BVH4BezierBuilderFast   (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Bezier1i>  (bvh,scene,listMode,0,0,false,sizeof(Bezier1i),1,1,true) {}
    template<> BVH4TriangleBuilderFast<Triangle1> ::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle1> (bvh,scene,listMode,0,0,false,sizeof(Triangle1),2,inf,true) {}
    template<> BVH4TriangleBuilderFast<Triangle4> ::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle4> (bvh,scene,listMode,2,2,false,sizeof(Triangle4),4,inf,true) {}
#if defined(__AVX__)
    template<> BVH4TriangleBuilderFast<Triangle8> ::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle8> (bvh,scene,listMode,3,2,false,sizeof(Triangle8),8,inf,true) {}
#endif
    template<> BVH4TriangleBuilderFast<Triangle1v>::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle1v>(bvh,scene,listMode,0,0,false,sizeof(Triangle1v),2,inf,true) {}
    template<> BVH4TriangleBuilderFast<Triangle4v>::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle4v>(bvh,scene,listMode,2,2,false,sizeof(Triangle4v),4,inf,true) {}
    template<> BVH4TriangleBuilderFast<Triangle4i>::BVH4TriangleBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<Triangle4i>(bvh,scene,listMode,2,2,true,sizeof(Triangle4i),4,inf,true) {}
    template<> BVH4UserGeometryBuilderFastT<AccelSetItem>::BVH4UserGeometryBuilderFastT (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<AccelSetItem>(bvh,scene,listMode,0,0,false,sizeof(AccelSetItem),1,1,true) {}

    template<> BVH4BezierBuilderFast  <Bezier1v>   ::BVH4BezierBuilderFast   (BVH4* bvh, BezierCurves* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Bezier1v>   (bvh,geom->parent,listMode,0,0,false,sizeof(Bezier1v)   ,1,1,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
    template<> BVH4BezierBuilderFast  <Bezier1i>  ::BVH4BezierBuilderFast   (BVH4* bvh, BezierCurves* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Bezier1i>  (bvh,geom->parent,listMode,0,0,false,sizeof(Bezier1i)  ,1,1,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
    template<> BVH4TriangleBuilderFast<Triangle1> ::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle1> (bvh,geom->parent,listMode,0,0,false,sizeof(Triangle1) ,2,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
    template<> BVH4TriangleBuilderFast<Triangle4> ::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle4> (bvh,geom->parent,listMode,2,2,false,sizeof(Triangle4) ,4,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
#if defined(__AVX__)
    template<> BVH4TriangleBuilderFast<Triangle8> ::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle8> (bvh,geom->parent,listMode,3,2,false,sizeof(Triangle8) ,8,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
#endif
    template<> BVH4TriangleBuilderFast<Triangle1v>::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle1v>(bvh,geom->parent,listMode,0,0,false,sizeof(Triangle1v),2,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
    template<> BVH4TriangleBuilderFast<Triangle4v>::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle4v>(bvh,geom->parent,listMode,2,2,false,sizeof(Triangle4v),4,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}
    template<> BVH4TriangleBuilderFast<Triangle4i>::BVH4TriangleBuilderFast (BVH4* bvh, TriangleMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<Triangle4i>(bvh,geom->parent,listMode,2,2,true ,sizeof(Triangle4i),4,inf,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}

    template<> BVH4UserGeometryBuilderFastT<AccelSetItem>::BVH4UserGeometryBuilderFastT (BVH4* bvh, UserGeometryBase* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<AccelSetItem>(bvh,geom->parent,listMode,0,0,false,sizeof(AccelSetItem),1,1,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}

    template<> BVH4SubdivBuilderFast<SubdivPatch1>::BVH4SubdivBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
      : geom(NULL), BVH4BuilderFastT<SubdivPatch1>(bvh,scene,listMode,0,0,false,sizeof(SubdivPatch1),1,1,true) {}
    template<> BVH4SubdivBuilderFast<SubdivPatch1>::BVH4SubdivBuilderFast (BVH4* bvh, SubdivMesh* geom, size_t listMode) 
      : geom(geom), BVH4BuilderFastT<SubdivPatch1>(bvh,geom->parent,listMode,0,0,false,sizeof(SubdivPatch1),1,1,geom->size() > THRESHOLD_FOR_SINGLE_THREADED) {}

    //template<> BVH4SubdivBuilderFast<SubdivPatchDispl1>::BVH4SubdivBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
    //: geom(NULL), BVH4BuilderFastT<SubdivPatchDispl1>(bvh,scene,listMode,0,0,false,sizeof(SubdivPatchDispl1),1,1,true) { this->bvh->alloc2.init(4096,4096); }
    
    BVH4SubdivQuadQuad4x4BuilderFast::BVH4SubdivQuadQuad4x4BuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
    : BVH4BuilderFastT<PrimRef>(bvh,scene,listMode,0,0,false,sizeof(QuadQuad4x4),1,1,true) { this->bvh->alloc2.init(4096,4096); }


    BVH4SubdivPatch1CachedBuilderFast::BVH4SubdivPatch1CachedBuilderFast (BVH4* bvh, Scene* scene, size_t listMode) 
    : BVH4BuilderFastT<PrimRef>(bvh,scene,listMode,0,0,false,sizeof(QuadQuad4x4),1,1,true) { this->bvh->alloc2.init(4096,4096); }
  
  
    BVH4TopLevelBuilderFastT::BVH4TopLevelBuilderFastT (LockStepTaskScheduler* scheduler, BVH4* bvh) 
      : prims_i(NULL), N(0), BVH4BuilderFast(scheduler,bvh,0,0,0,false,0,1,1) {}

    BVH4BuilderFast::~BVH4BuilderFast () 
    {
      if (prims) os_free(prims,bytesPrims); prims = NULL;
      bvh->alloc.shrink(); 
    }
    
    void BVH4BuilderFast::build(size_t threadIndex, size_t threadCount) 
    {
      /* calculate size of scene */
      size_t numPrimitivesOld = numPrimitives;
      bvh->numPrimitives = numPrimitives = number_of_primitives();
      bool parallel = needAllThreads && numPrimitives > THRESHOLD_FOR_SINGLE_THREADED;
	  
      /* initialize BVH */
      bvh->init(sizeof(BVH4::Node),numPrimitives, parallel ? (threadCount+1) : 1); // threadCount+1 for toplevel build

      /* skip build for empty scene */
      if (numPrimitives == 0) 
	return;
      
      /* verbose mode */
      if (g_verbose >= 1)
        std::cout << "building BVH4<" << bvh->primTy.name << "> with " << TOSTRING(isa) "::BVH4BuilderFast ... " << std::flush;
      
      /* allocate build primitive array */
      if (numPrimitivesOld != numPrimitives)
      {
	if (prims) os_free(prims,bytesPrims);
	bytesPrims = numPrimitives * sizeof(PrimRef);
        prims = (PrimRef* ) os_malloc(bytesPrims);  memset(prims,0,bytesPrims);
      }
      
      if (!parallel) {
	build_sequential(threadIndex,threadCount);
      } 
      else {
        state.reset(new GlobalState());
	//size_t numActiveThreads = threadCount;
	size_t numActiveThreads = min(threadCount,getNumberOfCores());
	build_parallel(threadIndex,numActiveThreads,0,1);
        state.reset(NULL);
      }
      
      /* verbose mode */
      if (g_verbose >= 2) {
	std::cout << "[DONE] " << 1000.0f*dt << "ms (" << numPrimitives/dt*1E-6 << " Mtris/s)" << std::endl;
	std::cout << BVH4Statistics(bvh).str();
      }

      /* benchmark mode */
      if (g_benchmark) {
	BVH4Statistics stat(bvh);
	std::cout << "BENCHMARK_BUILD " << dt << " " << double(numPrimitives)/dt << " " << stat.sah() << " " << stat.bytesUsed() << std::endl;
      }
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

    template<typename Primitive>
    size_t BVH4BezierBuilderFast<Primitive>::number_of_primitives() 
    {
      if (geom) return geom->size();
      else      return this->scene->numBezierCurves;
    }
    
    template<typename Primitive>
    void BVH4BezierBuilderFast<Primitive>::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      if (geom) PrimRefArrayGenFromGeometry<BezierCurves>::generate_sequential(threadIndex, threadCount, geom , this->prims, pinfo);
      else      PrimRefArrayGen                          ::generate_sequential(threadIndex, threadCount, this->scene, BEZIER_CURVES, 1, this->prims, pinfo);
    }

    template<typename Primitive>
    void BVH4BezierBuilderFast<Primitive>::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) 
    {
      if (geom) PrimRefArrayGenFromGeometry<BezierCurves>::generate_parallel(threadIndex, threadCount, scheduler, geom , this->prims, pinfo);
      else      PrimRefArrayGen                          ::generate_parallel(threadIndex, threadCount, scheduler, this->scene, BEZIER_CURVES, 1, this->prims, pinfo);
    }
    
    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

    template<typename Primitive>
    size_t BVH4TriangleBuilderFast<Primitive>::number_of_primitives() 
    {
      if (geom) return geom->numTriangles;
      else      return this->scene->numTriangles;
    }
    
    template<typename Primitive>
    void BVH4TriangleBuilderFast<Primitive>::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      if (geom) PrimRefArrayGenFromGeometry<TriangleMesh>::generate_sequential(threadIndex, threadCount, geom , this->prims, pinfo);
      else      PrimRefArrayGen                          ::generate_sequential(threadIndex, threadCount, this->scene, TRIANGLE_MESH, 1, this->prims, pinfo);
    }

    template<typename Primitive>
    void BVH4TriangleBuilderFast<Primitive>::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) 
    {
      if (geom) PrimRefArrayGenFromGeometry<TriangleMesh>::generate_parallel(threadIndex, threadCount, scheduler, geom , this->prims, pinfo);
      else      PrimRefArrayGen                          ::generate_parallel(threadIndex, threadCount, scheduler, this->scene, TRIANGLE_MESH, 1, this->prims, pinfo);
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

    template<typename Primitive>
    size_t BVH4UserGeometryBuilderFastT<Primitive>::number_of_primitives() 
    {
      if (geom) return geom->size();
      else      return this->scene->numUserGeometries1;
    }
    
    template<typename Primitive>
    void BVH4UserGeometryBuilderFastT<Primitive>::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      if (geom) PrimRefArrayGenFromGeometry<UserGeometryBase>::generate_sequential(threadIndex, threadCount, geom , this->prims, pinfo);
      else      PrimRefArrayGen                              ::generate_sequential(threadIndex, threadCount, this->scene, USER_GEOMETRY, 1, this->prims, pinfo);
    }

    template<typename Primitive>
    void BVH4UserGeometryBuilderFastT<Primitive>::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) 
    {
      if (geom) PrimRefArrayGenFromGeometry<UserGeometryBase>::generate_parallel(threadIndex, threadCount, scheduler, geom , this->prims, pinfo);
      else      PrimRefArrayGen                              ::generate_parallel(threadIndex, threadCount, scheduler, this->scene, USER_GEOMETRY, 1, this->prims, pinfo);
    }


    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

   template<typename Primitive>
   void BVH4SubdivBuilderFast<Primitive>::build(size_t threadIndex, size_t threadCount)
    {
      this->bvh->alloc2.reset();
      size_t numPatches = 0;
      for (size_t i=0; i<this->scene->size(); i++) 
      {
	const Geometry* geom = this->scene->get(i);
        if (geom == NULL || !geom->isEnabled()) continue;
	if (geom->type != SUBDIV_MESH) continue;
        SubdivMesh* subdiv_mesh = (SubdivMesh*)geom;
        subdiv_mesh->initializeHalfEdgeStructures();
	numPatches += subdiv_mesh->size();
      }
      BVH4BuilderFast::build(threadIndex,threadCount);
    }
 
    template<typename Primitive>
    size_t BVH4SubdivBuilderFast<Primitive>::number_of_primitives() 
    {
      if (geom) return geom->size();
      else      return this->scene->numSubdivPatches;
    }
    
    template<typename Primitive>
    void BVH4SubdivBuilderFast<Primitive>::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      if (geom) PrimRefArrayGenFromGeometry<SubdivMesh>::generate_sequential(threadIndex, threadCount, geom , this->prims, pinfo);
      else      PrimRefArrayGen                        ::generate_sequential(threadIndex, threadCount, this->scene, SUBDIV_MESH, 1, this->prims, pinfo);
    }

    template<typename Primitive>
    void BVH4SubdivBuilderFast<Primitive>::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) 
    {
      if (geom) PrimRefArrayGenFromGeometry<SubdivMesh>::generate_parallel(threadIndex, threadCount, scheduler, geom , this->prims, pinfo);
      else      PrimRefArrayGen                        ::generate_parallel(threadIndex, threadCount, scheduler, this->scene, SUBDIV_MESH, 1, this->prims, pinfo);
    }
    

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

   void BVH4SubdivQuadQuad4x4BuilderFast::build(size_t threadIndex, size_t threadCount)
    {
      /* initialize all half edge structures */
      new (&iter) Scene::Iterator<SubdivMesh>(this->scene);
      for (size_t i=0; i<iter.size(); i++)
        if (iter[i]) iter[i]->initializeHalfEdgeStructures();

      this->bvh->alloc2.reset();

      pstate.init(iter,size_t(1024));

      BVH4BuilderFast::build(threadIndex,threadCount);
    }

    size_t BVH4SubdivQuadQuad4x4BuilderFast::number_of_primitives() 
    {
      PrimInfo pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
      {
        size_t s = 0;
        for (size_t f=r.begin(); f!=r.end(); ++f) 
	{
          if (!mesh->valid(f)) continue;

	  feature_adaptive_subdivision_bspline(f,mesh->getHalfEdge(f),mesh->getVertexPositionPtr(),
					       [&](const CatmullClarkPatch& patch, const Vec2f uv[4], const int subdiv[4])
	  {
	    if (!patch.isRegular()) { s++; return; }
 	    const float l0 = patch.ring[0].edge_level;
	    const float l1 = patch.ring[1].edge_level;
	    const float l2 = patch.ring[2].edge_level;
	    const float l3 = patch.ring[3].edge_level;
	    const TessellationPattern pattern0(l0,subdiv[0]);
	    const TessellationPattern pattern1(l1,subdiv[1]);
	    const TessellationPattern pattern2(l2,subdiv[2]);
	    const TessellationPattern pattern3(l3,subdiv[3]);
	    const TessellationPattern pattern_x = pattern0.size() > pattern2.size() ? pattern0 : pattern2;
	    const TessellationPattern pattern_y = pattern1.size() > pattern3.size() ? pattern1 : pattern3;
	    const int nx = (pattern_x.size()+7)/8;
	    const int ny = (pattern_y.size()+7)/8;
	    s += nx*ny;
	  });
	}
        return PrimInfo(s,empty,empty);
      }, [](const PrimInfo& a, const PrimInfo b) { return PrimInfo(a.size()+b.size(),empty,empty); });

      return pinfo.size();
    }
    
    void BVH4SubdivQuadQuad4x4BuilderFast::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
      {
	PrimInfo s(empty);
        for (size_t f=r.begin(); f!=r.end(); ++f) {
          if (!mesh->valid(f)) continue;
	  
	  feature_adaptive_subdivision_bspline(f,mesh->getHalfEdge(f),mesh->getVertexPositionPtr(),
					       [&](const CatmullClarkPatch& patch, const Vec2f uv[4], const int subdiv[4])
	  {
	    size_t id = rand();

	    if (!patch.isRegular())
	    {
	      QuadQuad4x4* leaf = (QuadQuad4x4*) bvh->alloc2.malloc(sizeof(QuadQuad4x4),16);
	      new (leaf) QuadQuad4x4(id,mesh->id,f);
	      const BBox3fa bounds = leaf->quad(scene,patch,uv[0],uv[1],uv[2],uv[3]);
	      prims[base.size()+s.size()] = PrimRef(bounds,BVH4::encodeTypedLeaf(leaf,0));
	      s.add(bounds);
	      return;
	    }

	    //GregoryPatch patcheval; 
	    BSplinePatch patcheval;
	    patcheval.init(patch);
	    
	    const float l0 = patch.ring[0].edge_level;
	    const float l1 = patch.ring[1].edge_level;
	    const float l2 = patch.ring[2].edge_level;
	    const float l3 = patch.ring[3].edge_level;
	    const TessellationPattern pattern0(l0,subdiv[0]);
	    const TessellationPattern pattern1(l1,subdiv[1]);
	    const TessellationPattern pattern2(l2,subdiv[2]);
	    const TessellationPattern pattern3(l3,subdiv[3]);
	    const TessellationPattern pattern_x = pattern0.size() > pattern2.size() ? pattern0 : pattern2;
	    const TessellationPattern pattern_y = pattern1.size() > pattern3.size() ? pattern1 : pattern3;
	    const int nx = pattern_x.size();
	    const int ny = pattern_y.size();
	    	    
	    for (int y=0; y<ny; y+=8) {
	      for (int x=0; x<nx; x+=8) {
		QuadQuad4x4* leaf = (QuadQuad4x4*) bvh->alloc2.malloc(sizeof(QuadQuad4x4),16);
		new (leaf) QuadQuad4x4(id,mesh->id,f);
		const BBox3fa bounds = leaf->build(scene,patcheval,pattern0,pattern1,pattern2,pattern3,pattern_x,x,nx,pattern_y,y,ny,uv[0],uv[1],uv[2],uv[3]);
		prims[base.size()+s.size()] = PrimRef(bounds,BVH4::encodeTypedLeaf(leaf,0));
		s.add(bounds);
	      }
	    }
	  });
        }
        return s;
      }, [](PrimInfo a, const PrimInfo& b) { a.merge(b); return a; });
    }
    
    void BVH4SubdivQuadQuad4x4BuilderFast::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) {
      create_primitive_array_sequential(threadIndex, threadCount, pinfo);  // FIXME: parallelize
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================





    // =======================================================================================================
    // ============================ similar builder as for MIC ===============================================
    // =======================================================================================================

   void BVH4SubdivPatch1CachedBuilderFast::build(size_t threadIndex, size_t threadCount)
    {
      /* initialize all half edge structures */
      new (&iter) Scene::Iterator<SubdivMesh>(this->scene);
      for (size_t i=0; i<iter.size(); i++)
        if (iter[i]) iter[i]->initializeHalfEdgeStructures();

      pstate.init(iter,size_t(1024));

      if (this->bvh->data_mem != NULL) 
        {
          os_free( this->bvh->data_mem, this->bvh->size_data_mem );
          this->bvh->data_mem      = NULL;
          this->bvh->size_data_mem = 0;
        }
      BVH4BuilderFast::build(threadIndex,threadCount);
    }

    size_t BVH4SubdivPatch1CachedBuilderFast::number_of_primitives() 
    {
      PrimInfo pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
      {
        size_t s = 0;
        for (size_t f=r.begin(); f!=r.end(); ++f) 
	{          
          if (!mesh->valid(f)) continue;
          /* do feature-adaptive-based counting here */
          s++;
	}
        return PrimInfo(s,empty,empty);
      }, [](const PrimInfo& a, const PrimInfo b) { return PrimInfo(a.size()+b.size(),empty,empty); });

      return pinfo.size();
    }
    
    void BVH4SubdivPatch1CachedBuilderFast::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      PING;
      DBG_PRINT(  bvh->numPrimitives );

      assert( this->bvh->data_mem == NULL);

      for (size_t i=0; i<numPrimitives; i++) // FIXME: parallelize
        prims[i] = PrimRef(empty,-1,-1);

      std::cout << "ALLOCATING SUBDIVPATCH1CACHED MEMORY FOR " << numPrimitives << " PRIMITIVES" << std::endl;

      this->bvh->size_data_mem = sizeof(SubdivPatch1Cached) * numPrimitives;
      this->bvh->data_mem      = os_malloc( this->bvh->size_data_mem );        
        
      SubdivPatch1Cached *const subdiv_patches = (SubdivPatch1Cached *)this->bvh->data_mem;

      pinfo = parallel_for_for_prefix_sum( pstate, iter, PrimInfo(empty), [&](SubdivMesh* mesh, const range<size_t>& r, size_t k, const PrimInfo& base) -> PrimInfo
      {
        PrimInfo s(empty);
        for (size_t f=r.begin(); f!=r.end(); ++f) {
          if (!mesh->valid(f)) continue;
	  
          const unsigned int primID = f;
          const unsigned int geomID = mesh->id; /* HOW DO I GET THE MESH ID ??? */

          const unsigned int patchIndex = base.size()+s.size();
          const SubdivPatch1Cached patch = SubdivPatch1Cached(mesh->getHalfEdge(f),
                                                      mesh->getVertexPositionPtr(),
                                                      geomID, 
                                                      primID,
                                                      mesh);
          /* FIXME: use storent to write out subdivpatch data to memory */
          subdiv_patches[patchIndex] = patch;

          /* compute patch bounds */
          const BBox3fa bounds = patch.bounds(mesh);
	  prims[base.size()+s.size()] = PrimRef(bounds,patchIndex,0); // geomID,primID);
	  s.add(bounds);

          //DBG_PRINT( patchIndex );
          //DBG_PRINT( s );
          //DBG_PRINT( bounds );
	  //PRINT2(base+s,bounds);
          //s++;
        }
        return s;
      }, [](PrimInfo a, const PrimInfo& b) { a.merge(b); return a; });


      for (size_t i=0; i<numPrimitives; i++) { // FIXME: parallelize
        //pinfo.add(prims[i].bounds());
	//if (prims[i].bounds().lower.y > prims[i].bounds().upper.y) {
	//  PRINT(prims[i].bounds());
	//}
	PRINT2(i,prims[i].bounds());
	assert(prims[i].geomID() != -1);
	assert(prims[i].bounds().lower.x <= prims[i].bounds().upper.x);
	assert(prims[i].bounds().lower.y <= prims[i].bounds().upper.y);
	assert(prims[i].bounds().lower.z <= prims[i].bounds().upper.z);
      }

      DBG_PRINT( pinfo );

    }
    
    void BVH4SubdivPatch1CachedBuilderFast::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) {
      create_primitive_array_sequential(threadIndex, threadCount, pinfo);  // FIXME: parallelize
    }

    void BVH4SubdivPatch1CachedBuilderFast::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID)
    {
      PING;
      size_t items = current.size();
      assert(items <= 1);
      const unsigned int patchIndex = prims[current.begin].ID();
      DBG_PRINT(patchIndex);
      SubdivPatch1Cached *const subdiv_patches = (SubdivPatch1Cached *)this->bvh->data_mem;
      DBG_PRINT( &subdiv_patches[patchIndex] );
      *current.parent = bvh->encodeLeaf((char*)&subdiv_patches[patchIndex],1);
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================












    size_t BVH4TopLevelBuilderFastT::number_of_primitives() {
      return N;
    }
    
    void BVH4TopLevelBuilderFastT::create_primitive_array_sequential(size_t threadIndex, size_t threadCount, PrimInfo& pinfo)
    {
      for (size_t i=0; i<N; i++) {
	pinfo.add(prims_i[i].bounds(),prims_i[i].center2());
	prims[i] = prims_i[i];
      }
    }

    void BVH4TopLevelBuilderFastT::create_primitive_array_parallel  (size_t threadIndex, size_t threadCount, LockStepTaskScheduler* scheduler, PrimInfo& pinfo) 
    {
      for (size_t i=0; i<N; i++) {
	pinfo.add(prims_i[i].bounds(),prims_i[i].center2());
	prims[i] = prims_i[i];
      }
    }
 
    void BVH4TopLevelBuilderFastT::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID)
    {
      size_t items = current.size();
      assert(items <= 1);
      *current.parent = (BVH4::NodeRef) prims[current.begin].ID();
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

    template<typename Primitive>
    void BVH4BuilderFastT<Primitive>::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID)
    {
      size_t items = Primitive::blocks(current.size());
      size_t start = current.begin;
            
      /* allocate leaf node */
      Primitive* accel = (Primitive*) leafAlloc.malloc(items*sizeof(Primitive));
      *current.parent = bvh->encodeLeaf((char*)accel,listMode ? listMode : items);
      
      for (size_t i=0; i<items; i++) 
	accel[i].fill(prims,start,current.end,scene,listMode);
    }

    template<>
    void BVH4BuilderFastT<PrimRef>::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID) {
      if (current.size() != 1) THROW_RUNTIME_ERROR("bvh4_builder_fast: internal error");
      *current.parent = (BVH4::NodeRef) prims[current.begin].ID();
    }

#if 0 // FIXME: remove
    template<>
    void BVH4BuilderFastT<SubdivPatchDispl1>::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID)
    {
      size_t items = current.size();
      size_t start = current.begin;
      if (items != 1) THROW_RUNTIME_ERROR("SubdivPatchDispl1: internal error");
            
      /* allocate leaf node */
      SubdivPatchDispl1* accel = (SubdivPatchDispl1*) leafAlloc.malloc(sizeof(SubdivPatchDispl1));
            
      const PrimRef& prim = prims[start];
      const unsigned int last   = listMode && !prims;
      const unsigned int geomID = prim.geomID();
      const unsigned int primID = prim.primID();
      const SubdivMesh* const subdiv_mesh = scene->getSubdivMesh(geomID);
      new (accel) SubdivPatchDispl1(bvh->alloc2,
                                    *current.parent,
                                    scene, 
                                    subdiv_mesh->getHalfEdge(primID),
                                    subdiv_mesh->getVertexPositionPtr(),
                                    geomID,
                                    primID,
                                    SUBDIVISION_LEVEL_DISPL,
                                    true); 
      
      *current.parent = bvh->encodeLeaf((char*)accel,1);
    }
#endif

    void BVH4BuilderFast::createLeaf(BuildRecord& current, Allocator& nodeAlloc, Allocator& leafAlloc, size_t threadIndex, size_t threadCount)
    {
      if (current.depth > BVH4::maxBuildDepthLeaf) 
        THROW_RUNTIME_ERROR("depth limit reached");
      
      /* create leaf for few primitives */
      if (current.size() <= minLeafSize) {
        createSmallLeaf(current,leafAlloc,threadIndex);
        return;
      }
      
      /* first split level */
      BuildRecord record0, record1;
      splitFallback(prims,current,record0,record1);
      
      /* second split level */
      BuildRecord children[4];
      splitFallback(prims,record0,children[0],children[1]);
      splitFallback(prims,record1,children[2],children[3]);

      /* allocate node */
      Node* node = (Node*) nodeAlloc.malloc(sizeof(Node)); node->clear();
      *current.parent = bvh->encodeNode(node);
      
      /* recurse into each child */
      for (size_t i=0; i<4; i++) 
      {
        node->set(i,children[i].geomBounds);
        children[i].parent = &node->child(i);
        children[i].depth = current.depth+1;
        createLeaf(children[i],nodeAlloc,leafAlloc,threadIndex,threadCount);
      }
      BVH4::compact(node); // move empty nodes to the end
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================
    
    void BVH4BuilderFast::splitFallback(PrimRef * __restrict__ const primref, BuildRecord& current, BuildRecord& leftChild, BuildRecord& rightChild)
    {
      const unsigned int center = (current.begin + current.end)/2;
      
      CentGeomBBox3fa left; left.reset();
      for (size_t i=current.begin; i<center; i++)
        left.extend(primref[i].bounds());
      leftChild.init(left,current.begin,center);
      
      CentGeomBBox3fa right; right.reset();
      for (size_t i=center; i<current.end; i++)
        right.extend(primref[i].bounds());	
      rightChild.init(right,center,current.end);
    }

    __forceinline void BVH4BuilderFast::splitSequential(BuildRecord& current, BuildRecord& leftChild, BuildRecord& rightChild, const size_t threadID, const size_t numThreads)
    {
      /* calculate binning function */
      PrimInfo pinfo(current.size(),current.geomBounds,current.centBounds);
      ObjectPartition::Split split = ObjectPartition::find(prims,current.begin,current.end,pinfo,logBlockSize);
      
      /* if we cannot find a valid split, enforce an arbitrary split */
      if (unlikely(!split.valid())) splitFallback(prims,current,leftChild,rightChild);
      
      /* partitioning of items */
      else split.partition(prims, current.begin, current.end, leftChild, rightChild);
    }
    
    void BVH4BuilderFast::splitParallel(BuildRecord& current, BuildRecord& leftChild, BuildRecord& rightChild, const size_t threadID, const size_t numThreads)
    {
      /* use primitive array temporarily for parallel splits */
      PrimRef* tmp = (PrimRef*) bvh->alloc.curPtr();
      PrimInfo pinfo(current.begin,current.end,current.geomBounds,current.centBounds);

      /* parallel binning of centroids */
      const float sah = state->parallelBinner.find(pinfo,prims,tmp,logBlockSize,threadID,numThreads,scheduler);

      /* if we cannot find a valid split, enforce an arbitrary split */
      if (unlikely(sah == float(inf))) splitFallback(prims,current,leftChild,rightChild);
      
      /* parallel partitioning of items */
      else state->parallelBinner.partition(pinfo,tmp,prims,leftChild,rightChild,threadID,numThreads,scheduler);
    }
    
    __forceinline void BVH4BuilderFast::split(BuildRecord& current, BuildRecord& left, BuildRecord& right, const size_t mode, const size_t threadID, const size_t numThreads)
    {
      if (mode == BUILD_TOP_LEVEL) splitParallel(current,left,right,threadID,numThreads);		  
      else                         splitSequential(current,left,right,threadID,numThreads);
    }
    
    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================
    
    __forceinline void BVH4BuilderFast::recurse_continue(BuildRecord& current, Allocator& nodeAlloc, Allocator& leafAlloc, const size_t mode, const size_t threadID, const size_t numThreads)
    {
      if (mode == BUILD_TOP_LEVEL) {
      	state->heap.push(current);
      }
      else if (mode == RECURSE_PARALLEL && current.size() > THRESHOLD_FOR_SUBTREE_RECURSION) {
        if (!state->threadStack[threadID].push(current))
          recurse(current,nodeAlloc,leafAlloc,RECURSE_SEQUENTIAL,threadID,numThreads);
      }
      else
        recurse(current,nodeAlloc,leafAlloc,mode,threadID,numThreads);
    }
    
    void BVH4BuilderFast::recurse(BuildRecord& current, Allocator& nodeAlloc, Allocator& leafAlloc, const size_t mode, const size_t threadID, const size_t numThreads)
    {
      __aligned(64) BuildRecord children[BVH4::N];
      
      /* create leaf node */
      if (current.depth >= BVH4::maxBuildDepth || current.size() <= minLeafSize) {
        assert(mode != BUILD_TOP_LEVEL);
        createLeaf(current,nodeAlloc,leafAlloc,threadID,numThreads);
        return;
      }

      /* fill all 4 children by always splitting the one with the largest surface area */
      unsigned int numChildren = 1;
      children[0] = current;

      do {
        
        /* find best child with largest bounding box area */
        int bestChild = -1;
        float bestArea = neg_inf;
        for (unsigned int i=0; i<numChildren; i++)
        {
          /* ignore leaves as they cannot get split */
          if (children[i].size() <= minLeafSize)
            continue;
          
          /* remember child with largest area */
          if (children[i].sceneArea() > bestArea) { 
            bestArea = children[i].sceneArea();
            bestChild = i;
          }
        }
        if (bestChild == -1) break;
        
        /*! split best child into left and right child */
        __aligned(64) BuildRecord left, right;
        split(children[bestChild],left,right,mode,threadID,numThreads);
        
        /* add new children left and right */
	left.init(current.depth+1); 
	right.init(current.depth+1);
        children[bestChild] = children[numChildren-1];
        children[numChildren-1] = left;
        children[numChildren+0] = right;
        numChildren++;
        
      } while (numChildren < BVH4::N);

      /* create leaf node if no split is possible */
      if (numChildren == 1) {
        assert(mode != BUILD_TOP_LEVEL);
        createLeaf(current,nodeAlloc,leafAlloc,threadID,numThreads);
        return;
      }
      
      /* allocate node */
      Node* node = (Node*) nodeAlloc.malloc(sizeof(Node)); node->clear();
      *current.parent = bvh->encodeNode(node);
      
      /* recurse into each child */
      for (unsigned int i=0; i<numChildren; i++) 
      {  
        node->set(i,children[i].geomBounds);
        children[i].parent = &node->child(i);
        recurse_continue(children[i],nodeAlloc,leafAlloc,mode,threadID,numThreads);
      }
    }
    
    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================
    
    void BVH4BuilderFast::buildSubTrees(size_t threadID, size_t numThreads)
    {
      __aligned(64) Allocator nodeAlloc(&bvh->alloc);
      __aligned(64) Allocator leafAlloc(&bvh->alloc);
      
      while (true) 
      {
	BuildRecord br;
	if (!state->heap.pop(br))
        {
          /* global work queue empty => try to steal from neighboring queues */	  
          bool success = false;
          for (size_t i=0; i<numThreads; i++)
          {
            if (state->threadStack[(threadID+i)%numThreads].pop(br)) {
              success = true;
              break;
            }
          }
          /* found nothing to steal ? */
          if (!success) break; // FIXME: may loose threads
        }
        
        /* process local work queue */
	recurse(br,nodeAlloc,leafAlloc,RECURSE_PARALLEL,threadID,numThreads);
	while (state->threadStack[threadID].pop(br))
          recurse(br,nodeAlloc,leafAlloc,RECURSE_PARALLEL,threadID,numThreads);
      }
      _mm_sfence(); // make written leaves globally visible
    }

    void BVH4BuilderFast::build_sequential(size_t threadIndex, size_t threadCount) 
    {
      /* start measurement */
      double t0 = 0.0f;
      if (g_verbose >= 2) t0 = getSeconds();
      
      /* initialize node and leaf allocator */
      bvh->alloc.clear();
      __aligned(64) Allocator nodeAlloc(&bvh->alloc);
      __aligned(64) Allocator leafAlloc(&bvh->alloc);
     
      /* create prim refs */
      PrimInfo pinfo(empty);
      create_primitive_array_sequential(threadIndex, threadCount, pinfo);
      bvh->bounds = pinfo.geomBounds;

      /* create initial build record */
      BuildRecord br;
      br.init(pinfo,0,pinfo.size());
      br.depth = 1;
      br.parent = &bvh->root;

      /* build BVH in single thread */
      recurse(br,nodeAlloc,leafAlloc,RECURSE_SEQUENTIAL,threadIndex,threadCount);
      _mm_sfence(); // make written leaves globally visible

      /* stop measurement */
      if (g_verbose >= 2) dt = getSeconds()-t0;
    }

    void BVH4BuilderFast::build_parallel(size_t threadIndex, size_t threadCount, size_t taskIndex, size_t taskCount) 
    {
      /* start measurement */
      double t0 = 0.0f;
      if (g_verbose >= 2) t0 = getSeconds();

      /* calculate list of primrefs */
      PrimInfo pinfo(empty);
      create_primitive_array_parallel(threadIndex, threadCount, scheduler, pinfo);
      bvh->bounds = pinfo.geomBounds;

      /* initialize node and leaf allocator */
      bvh->alloc.clear();
      __aligned(64) Allocator nodeAlloc(&bvh->alloc);
      __aligned(64) Allocator leafAlloc(&bvh->alloc);

      /* create initial build record */
      BuildRecord br;
      br.init(pinfo,0,pinfo.size());
      br.depth = 1;
      br.parent = &bvh->root;
      
      /* initialize thread-local work stacks */
      for (size_t i=0; i<threadCount; i++)
        state->threadStack[i].reset();
      
      /* push initial build record to global work stack */
      state->heap.reset();
      state->heap.push(br);

      /* work in multithreaded toplevel mode until sufficient subtasks got generated */
      while (state->heap.size() < 2*threadCount)
      {
        BuildRecord br;

        /* pop largest item for better load balancing */
	if (!state->heap.pop(br)) 
          break;
        
        /* guarantees to create no leaves in this stage */
        if (br.size() <= max(minLeafSize,THRESHOLD_FOR_SINGLE_THREADED)) {
	  state->heap.push(br);
          break;
	}

        recurse(br,nodeAlloc,leafAlloc,BUILD_TOP_LEVEL,threadIndex,threadCount);
      }
      _mm_sfence(); // make written leaves globally visible

      std::sort(state->heap.begin(),state->heap.end(),BuildRecord::Greater());

      /* now process all created subtasks on multiple threads */
      scheduler->dispatchTask(task_buildSubTrees, this, threadIndex, threadCount );
      
      /* stop measurement */
      if (g_verbose >= 2) dt = getSeconds()-t0;
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================

    BVH4BuilderFastGeneric::BVH4BuilderFastGeneric (BVH4* bvh, PrimRef* prims, size_t N, const MakeLeaf& makeLeaf, size_t listMode, 
                                                    size_t logBlockSize, size_t logSAHBlockSize, bool needVertices, size_t primBytes, const size_t minLeafSize, const size_t maxLeafSize)
      : makeLeaf(makeLeaf), BVH4BuilderFast(NULL,bvh,listMode,logBlockSize,logSAHBlockSize,needVertices,primBytes,minLeafSize,maxLeafSize)
    {
      this->prims = prims;
      this->N = N;
    }

    BVH4BuilderFastGeneric::~BVH4BuilderFastGeneric () {
      prims = NULL;
    }

    void BVH4BuilderFastGeneric::build(size_t threadIndex, size_t threadCount)
    {
      bvh->init(sizeof(BVH4::Node),N*N,1);
      
      /* initialize node and leaf allocator */
      bvh->alloc.clear();
      __aligned(64) Allocator nodeAlloc(&bvh->alloc);
      __aligned(64) Allocator leafAlloc(&bvh->alloc);
     
      /* calculate bounding box */
      PrimInfo pinfo(empty);
      for (size_t i=0; i<N; i++) pinfo.add(prims[i].bounds());
      bvh->bounds = pinfo.geomBounds;

      /* create initial build record */
      BuildRecord br;
      br.init(pinfo,0,pinfo.size());
      br.depth = 1;
      br.parent = &bvh->root;

      /* build BVH in single thread */
      recurse(br,nodeAlloc,leafAlloc,RECURSE_SEQUENTIAL,threadIndex,threadCount);
      _mm_sfence(); // make written leaves globally visible
    }

    void BVH4BuilderFastGeneric::createSmallLeaf(BuildRecord& current, Allocator& leafAlloc, size_t threadID) 
    {
      *current.parent = makeLeaf(leafAlloc, &prims[current.begin], current.size());
    }

    // =======================================================================================================
    // =======================================================================================================
    // =======================================================================================================
    
    Builder* BVH4Bezier1vBuilderFast    (void* bvh, Scene* scene, size_t mode) { return new class BVH4BezierBuilderFast<Bezier1v>  ((BVH4*)bvh,scene,mode); }
    Builder* BVH4Bezier1iBuilderFast   (void* bvh, Scene* scene, size_t mode) { return new class BVH4BezierBuilderFast<Bezier1i>((BVH4*)bvh,scene,mode); }
    Builder* BVH4Triangle1BuilderFast  (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle1> ((BVH4*)bvh,scene,mode); }
    Builder* BVH4Triangle4BuilderFast  (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4> ((BVH4*)bvh,scene,mode); }
#if defined(__AVX__)
    Builder* BVH4Triangle8BuilderFast  (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle8> ((BVH4*)bvh,scene,mode); }
#endif
    Builder* BVH4Triangle1vBuilderFast (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle1v>((BVH4*)bvh,scene,mode); }
    Builder* BVH4Triangle4vBuilderFast (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4v>((BVH4*)bvh,scene,mode); }
    Builder* BVH4Triangle4iBuilderFast (void* bvh, Scene* scene, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4i>((BVH4*)bvh,scene,mode); }
    Builder* BVH4UserGeometryBuilderFast(void* bvh, Scene* scene, size_t mode) { return new class BVH4UserGeometryBuilderFastT<AccelSetItem>((BVH4*)bvh,scene,mode); }

    Builder* BVH4Bezier1vMeshBuilderFast    (void* bvh, BezierCurves* geom, size_t mode) { return new class BVH4BezierBuilderFast<Bezier1v>  ((BVH4*)bvh,geom,mode); }
    Builder* BVH4Bezier1iMeshBuilderFast   (void* bvh, BezierCurves* geom, size_t mode) { return new class BVH4BezierBuilderFast<Bezier1i> ((BVH4*)bvh,geom,mode); }
    Builder* BVH4Triangle1MeshBuilderFast  (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle1> ((BVH4*)bvh,mesh,mode); }
    Builder* BVH4Triangle4MeshBuilderFast  (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4> ((BVH4*)bvh,mesh,mode); }
#if defined(__AVX__)
    Builder* BVH4Triangle8MeshBuilderFast  (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle8> ((BVH4*)bvh,mesh,mode); }
#endif
    Builder* BVH4Triangle1vMeshBuilderFast (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle1v>((BVH4*)bvh,mesh,mode); }
    Builder* BVH4Triangle4vMeshBuilderFast (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4v>((BVH4*)bvh,mesh,mode); }
    Builder* BVH4Triangle4iMeshBuilderFast (void* bvh, TriangleMesh* mesh, size_t mode) { return new class BVH4TriangleBuilderFast<Triangle4i>((BVH4*)bvh,mesh,mode); }
    Builder* BVH4UserGeometryMeshBuilderFast   (void* bvh, UserGeometryBase* geom, size_t mode) { return new class BVH4UserGeometryBuilderFastT<AccelSetItem>((BVH4*)bvh,geom,mode); }

    Builder* BVH4SubdivPatch1BuilderFast(void* bvh, Scene* scene, size_t mode) { return new class BVH4SubdivBuilderFast<SubdivPatch1>((BVH4*)bvh,scene,mode); }
    Builder* BVH4SubdivQuadQuad4x4BuilderFast(void* bvh, Scene* scene, size_t mode) { return new class BVH4SubdivQuadQuad4x4BuilderFast((BVH4*)bvh,scene,mode); }
    Builder* BVH4SubdivPatch1CachedBuilderFast(void* bvh, Scene* scene, size_t mode) { return new class BVH4SubdivPatch1CachedBuilderFast((BVH4*)bvh,scene,mode); }

  }
}
