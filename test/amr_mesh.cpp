/** This file is part of VLSV file format.
 * 
 *  Copyright 2011-2014 Finnish Meteorological Institute
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>
#include <iostream>
#include <cmath>
#include <map>
#include <algorithm>

#include <vlsv_writer.h>

#include "amr_mesh.h"

using namespace std;

namespace amr {

   namespace limits {
      enum meshlimits {
	 XMIN,
	 XMAX,
	 YMIN,
	 YMAX,
	 ZMIN,
	 ZMAX
      };
   }
   
AmrMesh::AmrMesh(const uint32_t& Nx0,const uint32_t& Ny0,const uint32_t& Nz0,const uint32_t& xCells,
		 const uint32_t& yCells,const uint32_t& zCells,const uint8_t& maxRefLevel) {
   bbox[0] = Nx0;
   bbox[1] = Ny0;
   bbox[2] = Nz0;
   N_blocks0       = bbox[0]*bbox[1]*bbox[2];

   bbox[3]    = xCells;
   bbox[4]    = yCells;
   bbox[5]    = zCells;
   refLevelMaxAllowed = maxRefLevel;
   
   initialized = false;

   callbackCoarsenBlock = NULL;
   callbackCreateBlock = NULL;
   callbackDeleteBlock = NULL;
   callbackRefineBlock = NULL;
}

AmrMesh::~AmrMesh() { 
   if (finalize() == false) {
      cerr << "AmrMesh warning: finalize() returned false" << endl;
   }
}

/** Get a const iterator pointing to the first existing block in mesh.
 * @return Iterator pointing to the first existing block.*/
std::unordered_map<GlobalID,LocalID>::iterator AmrMesh::begin() {
   return globalIDs.begin();
}

bool AmrMesh::checkBlock(const GlobalID& globalID) {
   // Test if the block exists:
   if (globalIDs.find(globalID) != globalIDs.end()) return true;

   // Recursively test if the block is refined, i.e., all its children exist:
   bool ok = true;
   vector<GlobalID> children;
   getChildren(globalID,children);
   for (size_t c=0; c<children.size(); ++c) {
      // Check that children are ok:
      if (checkBlock(children[c]) == false) {
	 ok = false;
      }
   }

   return ok;
}

bool AmrMesh::checkMesh() {
   bool ok = true;

   for (std::unordered_map<GlobalID,LocalID>::const_iterator it=begin(); it!=end(); ++it) {
      // Recursively test that all siblings are ok. Note that vector siblings
      // also contains the global ID of this block:
      vector<GlobalID> siblings;
      getSiblings(it->first,siblings);
      for (size_t s=0; s<siblings.size(); ++s) {
	 if (checkBlock(siblings[s]) == false) ok = false;
      }
   }

   return ok;
}

/** Attempt to coarsen the given block. Coarsen will not succeed if it 
 * would create a block with more than one refinement level difference 
 * between it and its neighbors.
 * @param globalID Global ID of the block.
 * @return If true, block was coarsened.*/
bool AmrMesh::coarsen(const GlobalID& globalID) {
   if (globalIDs.find(globalID) == globalIDs.end()) return false;
   
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);
   if (refLevel == 0) return false;

   vector<GlobalID> nbrs;
   getSiblingNeighbors(globalID,nbrs);

   // Check if block can be coarsened:
   for (size_t n=0; n<nbrs.size(); ++n) {
      vector<GlobalID> children;
      getChildren(nbrs[n],children);
      for (size_t c=0; c<children.size(); ++c) {
         if (globalIDs.find(children[c]) != globalIDs.end()) return false;
      }
   }
   
   // Calculate the global ID of the block and its siblings,
   // and call the user-defined callback:
   GlobalID siblings[8];
   getSiblings(globalID,siblings);
   for (size_t s=0; s<8; ++s) {
      if (globalIDs.find(siblings[s]) == globalIDs.end()) {
         return false;
      }
   }
   
   LocalID siblingIndices[8];
   for (int s=0; s<8; ++s) {
      unordered_map<GlobalID,LocalID>::const_iterator it = globalIDs.find(siblings[s]);
      if (it == globalIDs.end()) {
         siblingIndices[s] = INVALID_LOCALID;
         continue;
      } else {
         siblingIndices[s] = it->second;
      }
   }
   
   LocalID newLocalID;
   if (callbackCoarsenBlock != NULL) {
      (*callbackCoarsenBlock)(siblings,siblingIndices,getParent(globalID),newLocalID);
   }

   // Remove the block and its siblings:
   for (size_t s=0; s<8; ++s) {
      if (globalIDs.find(siblings[s]) == globalIDs.end()) {
         cerr << "ERROR coarsen trying to remove non-existing block " << siblings[s] << endl;
         exit(1);
      }
      globalIDs.erase(siblings[s]);
   }

   // Insert parent:
   globalIDs.insert(make_pair(getParent(globalID),newLocalID));
   return true;
}

/** Get a const iterator pointing past the last existing block.
 * @param Iterator pointing past the last existing block.*/
std::unordered_map<GlobalID,LocalID>::iterator AmrMesh::end() {
   return globalIDs.end();
}

/** Finalize the class. Deallocates all memory.
 * @return If true, class finalized correctly.*/
bool AmrMesh::finalize() {
   bool success = true;
   if (callbackDeleteBlock != NULL) {
      for (unordered_map<GlobalID,LocalID>::iterator it=globalIDs.begin(); it!=globalIDs.end(); ++it) {
	 if ((*callbackDeleteBlock)(it->first,it->second) == false) success = false;
      }
   }
   return success;
}

LocalID AmrMesh::get(const GlobalID& globalID) const {
   // If block does not exist, return invalid local ID:
   unordered_map<GlobalID,LocalID>::const_iterator it = globalIDs.find(globalID);
   if (it == globalIDs.end()) return INVALID_LOCALID;

   // Return block's local ID:
   return it->second;
}

/** Get the global ID of an existing block that contains given coordinates.
 */
GlobalID AmrMesh::getGlobalID(const double& x,const double& y,const double& z) {
   // Check that given coordinates are not outside the mesh:
   if (x < meshLimits[limits::XMIN] || x > meshLimits[limits::XMAX]) return INVALID_GLOBALID;
   if (y < meshLimits[limits::YMIN] || x > meshLimits[limits::YMAX]) return INVALID_GLOBALID;
   if (z < meshLimits[limits::ZMIN] || x > meshLimits[limits::ZMAX]) return INVALID_GLOBALID;

   for (int r=0; r<=refLevelMaxAllowed; ++r) {
      // Calculate the number of blocks in each coordinate direction on this refinement level:
      const uint32_t Nx = bbox[0]*(r+1);
      const uint32_t Ny = bbox[1]*(r+1);
      const uint32_t Nz = bbox[2]*(r+1);

      // Calculate block size:
      const double dx = (meshLimits[limits::XMAX]-meshLimits[limits::XMIN]) / Nx;
      const double dy = (meshLimits[limits::YMAX]-meshLimits[limits::YMIN]) / Ny;
      const double dz = (meshLimits[limits::ZMAX]-meshLimits[limits::ZMIN]) / Nz;

      // Calculate the (i,j,k) indices of the block containing the given coordinates:
      const uint32_t i = static_cast<uint32_t>((x-meshLimits[limits::XMIN]) / dx);
      const uint32_t j = static_cast<uint32_t>((y-meshLimits[limits::YMIN]) / dy);
      const uint32_t k = static_cast<uint32_t>((z-meshLimits[limits::ZMIN]) / dz);
      
      const GlobalID gID = getGlobalID(r,i,j,k);
      if (globalIDs.find(gID) != globalIDs.end()) return gID;
   }
   return INVALID_GLOBALID;
}

bool AmrMesh::getBlockCoordinates(const GlobalID& globalID,double coords[3]) const {
   // Exit if block does not exist:
   unordered_map<GlobalID,LocalID>::const_iterator it = globalIDs.find(globalID);
   if (it == globalIDs.end()) return false;

   // Calculate block's refinement level and (i,j,k) indices:
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);
   
   getBlockSize(globalID,coords);
   coords[0] = meshLimits[limits::XMIN] + i*coords[0];
   coords[1] = meshLimits[limits::YMIN] + j*coords[1];
   coords[2] = meshLimits[limits::ZMIN] + k*coords[2];
   return true;
}
   
bool AmrMesh::getBlockSize(const GlobalID& globalID,double size[3]) const {
   // Calculate block's refinement level and (i,j,k) indices:
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);

   // Calculate the number of blocks in each coordinate direction on this refinement level:
   const uint32_t Nx = bbox[0]*(refLevel+1);
   const uint32_t Ny = bbox[1]*(refLevel+1);
   const uint32_t Nz = bbox[2]*(refLevel+1);

   // Calculate the number of blocks in each coordinate direction on this refinement level:
   size[0] = (meshLimits[limits::XMAX]-meshLimits[limits::XMIN]) / Nx;
   size[1] = (meshLimits[limits::YMAX]-meshLimits[limits::YMIN]) / Ny;
   size[2] = (meshLimits[limits::ZMAX]-meshLimits[limits::ZMIN]) / Nz;
}

/** Get global IDs of block's children. Note that the children may 
 * or may not exists -- this function simply calculates the global IDs.
 * @param globalID Global ID of the block.
 * @param children Vector where global IDs of children are inserted.*/
void AmrMesh::getChildren(const GlobalID& globalID,std::vector<GlobalID>& children) {
   children.clear();
   
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);
   if (refLevel+1 > refLevelMaxAllowed) return;
   
   i *= 2;
   j *= 2;
   k *= 2;
   
   children.push_back(getGlobalID(refLevel+1,i  ,j  ,k  ));
   children.push_back(getGlobalID(refLevel+1,i+1,j  ,k  ));
   children.push_back(getGlobalID(refLevel+1,i  ,j+1,k  ));
   children.push_back(getGlobalID(refLevel+1,i+1,j+1,k  ));
   children.push_back(getGlobalID(refLevel+1,i  ,j  ,k+1));
   children.push_back(getGlobalID(refLevel+1,i+1,j  ,k+1));
   children.push_back(getGlobalID(refLevel+1,i  ,j+1,k+1));
   children.push_back(getGlobalID(refLevel+1,i+1,j+1,k+1));
}

/** Get the global ID of a block with the given indices and refinement level. 
 * The refinement level must be equal or greater than zero, and less than or equal 
 * to the maximum refinement level.
 * @param refLevel Block's refinement level.
 * @param i i-index of the block at given refinement level.
 * @param j j-index of the block at given refinement level.
 * @param k k-index of the block at given refinement level.
 * @return Global ID of the block.*/
GlobalID AmrMesh::getGlobalID(const uint32_t& refLevel,const uint32_t& i,const uint32_t& j,const uint32_t& k) {
   const uint32_t multiplier = pow(2,refLevel);
   return offsets[refLevel] + k*bbox[1]*bbox[0]*multiplier*multiplier + j*bbox[0]*multiplier + i;
}

/** Get i,j,k indices of the block with given global ID, and its refinement level.
 * @param globalID Global ID of the block.
 * @param refLevel Block's refinement level is written here.
 * @param i Block's i-index is written here.
 * @param j Block's j-index is written here.
 * @param k Block's k-index is written here.*/
void AmrMesh::getIndices(const GlobalID& globalID,uint32_t& refLevel,uint32_t& i,uint32_t& j,uint32_t& k) const {
   refLevel   = upper_bound(offsets.begin(),offsets.end(),globalID)-offsets.begin()-1;
   const GlobalID cellOffset = offsets[refLevel];

   const GlobalID multiplier = pow(2,refLevel);
   const GlobalID Nx = bbox[0] * multiplier;
   const GlobalID Ny = bbox[1] * multiplier;

   GlobalID index = globalID - cellOffset;
   k = index / (Ny*Nx);
   index -= k*Ny*Nx;
   j = index / Nx;
   i = index - j*Nx;
}

/** Get global IDs of block's neighbors. The neighbor IDs are calculated at the 
 * same refinement level as the block. Thus, some of the returned neighbors may 
 * not actually exist.
 * @param globalID Global ID of the block.
 * @param neighboIDs Vector where neighbors global IDs are written to.*/
void AmrMesh::getNeighbors(const GlobalID& globalID,std::vector<GlobalID>& neighborIDs) {
   neighborIDs.clear();

   uint32_t i,j,k,refLevel;
   getIndices(globalID,refLevel,i,j,k);

   const uint32_t Nx_max = bbox[0] * pow(2,refLevel);
   const uint32_t Ny_max = bbox[1] * pow(2,refLevel);
   const uint32_t Nz_max = bbox[2] * pow(2,refLevel);
   for (int k_off=-1; k_off<2; ++k_off) {
      if (k+k_off >= Nz_max) continue;
      for (int j_off=-1; j_off<2; ++j_off) {
	 if (j+j_off >= Ny_max) continue;
	 for (int i_off=-1; i_off<2; ++i_off) {
	    if (i+i_off >= Nx_max) continue;
	    if (i_off == 0 && (j_off == 0 && k_off == 0)) continue;
	    neighborIDs.push_back(getGlobalID(refLevel,i+i_off,j+j_off,k+k_off));
	 }
      }
   }
}

/** Get global ID of block's parent. If the block is at refinement level 0, 
 * i.e., at the base grid level, block's global ID is returned instead.
 * @param globalID Global ID of the block.
 * @return Global ID of block's parent.*/
GlobalID AmrMesh::getParent(const GlobalID& globalID) {
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);

   if (refLevel == 0) return globalID;
   
   i /= 2;
   j /= 2;
   k /= 2;
   return getGlobalID(refLevel-1,i,j,k);
}

/** Get global IDs of all neighbors of this block and its siblings 
 * at the same refinement level as the block.
 * If the block is not at the boundary of the simulation domain, the 
 * returned vector should contain 56 neighbor IDs.
 * @param globalID Global ID of the block.
 * @param nbrs Vector where the neighbor IDs are written to.*/
void AmrMesh::getSiblingNeighbors(const GlobalID& globalID,std::vector<GlobalID>& nbrs) {
   nbrs.clear();

   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);

   i -= (i % 2);
   j -= (j % 2);
   k -= (k % 2);
   
   for (int k_off=-1; k_off<3; ++k_off) {
     for (int j_off=-1; j_off<3; ++j_off) {
       for (int i_off=-1; i_off<3; ++i_off) {
          int cntr=0;
          if (i_off == 0 || i_off == 1) ++cntr;
          if (j_off == 0 || j_off == 1) ++cntr;
          if (k_off == 0 || k_off == 1) ++cntr;
          if (cntr == 3) continue;
          nbrs.push_back(getGlobalID(refLevel,i+i_off,j+j_off,k+k_off));
       }
     }
   }
}
   
   void AmrMesh::getSiblings(const GlobalID& globalID,GlobalID siblings[8]) {
   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);
   
   i -= (i % 2);
   j -= (j % 2);
   k -= (k % 2);
   
   siblings[0] = getGlobalID(refLevel,i  ,j  ,k  );
   siblings[1] = getGlobalID(refLevel,i+1,j  ,k  );
   siblings[2] = getGlobalID(refLevel,i  ,j+1,k  );
   siblings[3] = getGlobalID(refLevel,i+1,j+1,k  );
   siblings[4] = getGlobalID(refLevel,i  ,j  ,k+1);
   siblings[5] = getGlobalID(refLevel,i+1,j  ,k+1);
   siblings[6] = getGlobalID(refLevel,i  ,j+1,k+1);
   siblings[7] = getGlobalID(refLevel,i+1,j+1,k+1);
}
   
/** Get global IDs of block's siblings at the same refinement level as the block. 
 * Note that some of the siblings may not exists, for example, 
 * if the sibling has been refined. Returned vector also contains the ID of this block.
 * @param globalID Global ID of the block.
 * @param siblings Vector where sibling's global IDs are written to.*/
void AmrMesh::getSiblings(const GlobalID& globalID,std::vector<GlobalID>& siblings) {
   siblings.clear();

   uint32_t refLevel,i,j,k;
   getIndices(globalID,refLevel,i,j,k);

   i -= (i % 2);
   j -= (j % 2);
   k -= (k % 2);
   
   siblings.push_back(getGlobalID(refLevel,i  ,j  ,k  ));
   siblings.push_back(getGlobalID(refLevel,i+1,j  ,k  ));
   siblings.push_back(getGlobalID(refLevel,i  ,j+1,k  ));
   siblings.push_back(getGlobalID(refLevel,i+1,j+1,k  ));
   siblings.push_back(getGlobalID(refLevel,i  ,j  ,k+1));
   siblings.push_back(getGlobalID(refLevel,i+1,j  ,k+1));
   siblings.push_back(getGlobalID(refLevel,i  ,j+1,k+1));
   siblings.push_back(getGlobalID(refLevel,i+1,j+1,k+1));
}

/** Initialize the mesh.
 * @param xmin
 * @param xmax
 * @param ymin
 * @param ymax
 * @param zmin
 * @param zmax
 * @return If true, mesh was successfully initialized and is ready for use.*/
bool AmrMesh::initialize(const double& xmin,const double& xmax,const double& ymin,const double& ymax,
			 const double& zmin,const double& zmax,const uint8_t refLevel) {   
   if (initialized == true) return true;
   initialized = false;
   if (refLevel > refLevelMaxAllowed) return false;

   // Calculate block global ID offsets for each refinement level:
   offsets.resize(refLevelMaxAllowed+1);
   offsets[0] = 0;
   for (size_t i=1; i<refLevelMaxAllowed+1; ++i) {
      offsets[i] = offsets[i-1] + N_blocks0 * pow(8,i-1);
   }

   // Insert all blocks at given refinement level to mesh:
   const uint32_t factor = pow(2,refLevel);
   
   size_t counter = 0;
   for (uint32_t k=0; k<bbox[2]*factor; ++k) 
     for (uint32_t j=0; j<bbox[1]*factor; ++j)
       for (uint32_t i=0; i<bbox[0]*factor; ++i) {
	  
	  if (1.0*rand()/RAND_MAX < 0.4) continue;
	  
	  GlobalID globalID = getGlobalID(refLevel,i,j,k);
	  LocalID localID = INVALID_LOCALID;
	  if (callbackCreateBlock != NULL) {
	     (*callbackCreateBlock)(globalID,localID);
	  }
	  globalIDs.insert(make_pair(globalID,localID));
       }

   meshLimits[limits::XMIN] = xmin;
   meshLimits[limits::XMAX] = xmax;
   meshLimits[limits::YMIN] = ymin;
   meshLimits[limits::YMAX] = ymax;
   meshLimits[limits::ZMIN] = zmin;
   meshLimits[limits::ZMAX] = zmax;
   
   initialized = true;
   return initialized;
}

/** Refine the block. This function will additionally refine block's neighbors, 
 * if it is necessary to maintain maximum difference of one refinement level 
 * between neighboring blocks.
 * @param globalID Global ID of the block.
 * @return If true, block was refined.*/
bool AmrMesh::refine(const GlobalID& globalID) {
   if (globalIDs.find(globalID) == globalIDs.end()) return false;

   vector<GlobalID> nbrs;
   getNeighbors(globalID,nbrs);

   uint32_t i,j,k,refLevel;
   getIndices(globalID,refLevel,i,j,k);
   if (refLevel == refLevelMaxAllowed) {
      return false;
   }

   // Store children global IDs to array:
   GlobalID childrenGlobalIDs[8];
   childrenGlobalIDs[0] = getGlobalID(refLevel+1,i  ,j  ,k  );
   childrenGlobalIDs[1] = getGlobalID(refLevel+1,i+1,j  ,k  );
   childrenGlobalIDs[2] = getGlobalID(refLevel+1,i  ,j+1,k  );
   childrenGlobalIDs[3] = getGlobalID(refLevel+1,i+1,j+1,k  );
   childrenGlobalIDs[4] = getGlobalID(refLevel+1,i  ,j  ,k+1);
   childrenGlobalIDs[5] = getGlobalID(refLevel+1,i+1,j  ,k+1);
   childrenGlobalIDs[6] = getGlobalID(refLevel+1,i  ,j+1,k+1);
   childrenGlobalIDs[7] = getGlobalID(refLevel+1,i+1,j+1,k+1);

   // Call refine callback:
   LocalID childrenLocalIDs[8];
   if (callbackRefineBlock != NULL) {
      (*callbackRefineBlock)(globalID,globalIDs[globalID],childrenGlobalIDs,childrenLocalIDs);
   }

   i *= 2;
   j *= 2;
   k *= 2;
   globalIDs.erase(globalID);
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i  ,j  ,k  ),childrenLocalIDs[0]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i+1,j  ,k  ),childrenLocalIDs[1]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i  ,j+1,k  ),childrenLocalIDs[2]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i+1,j+1,k  ),childrenLocalIDs[3]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i  ,j  ,k+1),childrenLocalIDs[4]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i+1,j  ,k+1),childrenLocalIDs[5]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i  ,j+1,k+1),childrenLocalIDs[6]));
   globalIDs.insert(make_pair(getGlobalID(refLevel+1,i+1,j+1,k+1),childrenLocalIDs[7]));

   // Enforce that neighbors have at maximum one refinement level difference:
   for (size_t n=0; n<nbrs.size(); ++n) {
      // Get parent of the neighbor:
      const GlobalID parentID = getParent(nbrs[n]);
      
      // If the parent exists, it is at two refinement levels higher than 
      // the block that was refined above. Refine parent of neighbor:
      if (parentID == nbrs[n]) continue;
      unordered_map<GlobalID,LocalID>::iterator parent = globalIDs.find(parentID);
      if (parent != globalIDs.end()) {
         refine(parentID);
      }
   }

   return true;
}
   
bool AmrMesh::registerCallbacks(CallbackCoarsenBlock coarsenBlock,CallbackCreateBlock createBlock,
				CallbackDeleteBlock deleteBlock,CallbackRefineBlock refineBlock) {
   cout << "registerCallbacks called" << endl;
   cout << "\t" << coarsenBlock << '\t' << createBlock << '\t' << deleteBlock << '\t' << refineBlock << endl;
   
   callbackCoarsenBlock = coarsenBlock;
   callbackCreateBlock  = createBlock;
   callbackDeleteBlock  = deleteBlock;
   callbackRefineBlock  = refineBlock;
   return true;
}

bool AmrMesh::set(const GlobalID& globalID,const LocalID& localID) {
   // Exit if block does not exist:
   unordered_map<GlobalID,LocalID>::iterator it=globalIDs.find(globalID);
   if (it == globalIDs.end()) return false;

   // Set new value for the block:
   it->second = localID;
   return true;
}

/** Get the number of blocks in the mesh.
 * @return Number of blocks in the mesh.*/
size_t AmrMesh::size() const {
   return globalIDs.size();
}

/** Write the mesh to given file.
 * @param fileName Name of the output file.
 * @return If true, mesh was successfully written.*/
bool AmrMesh::write(const std::string fileName) {
   if (initialized == false) return false;
   bool success = true;

   const string meshName = "amr_mesh";
   const string filename = "cylinder.vlsv";
   
   vlsv::Writer vlsv;
   if (vlsv.open(fileName,MPI_COMM_WORLD,0) == false) {
      success = false;
      return false;
   }

   // Write block global IDs:
   map<string,string> attributes;
   attributes["name"] = meshName;
   attributes["type"] = vlsv::mesh::STRING_UCD_AMR;
   stringstream ss;
   ss << (uint32_t)refLevelMaxAllowed;
   attributes["max_refinement_level"] = ss.str();
   attributes["geometry"] = vlsv::geometry::STRING_CARTESIAN;
     {
	vector<GlobalID> buffer;
	for (unordered_map<GlobalID,LocalID>::iterator it=globalIDs.begin(); it!=globalIDs.end(); ++it) {
	   buffer.push_back(it->first);
	}
	if (vlsv.writeArray("MESH",attributes,globalIDs.size(),1,&(buffer[0])) == false) success = false;
     }

   // Write mesh bounding box:
   attributes.clear();
   attributes["mesh"] = meshName;
   if (vlsv.writeArray("MESH_BBOX",attributes,6,1,bbox) == false) success = false;
   
   // Write domain sizes:
   uint64_t domainSize[2];
   domainSize[0] = globalIDs.size();
   domainSize[1] = 0;
   if (vlsv.writeArray("MESH_DOMAIN_SIZES",attributes,1,2,domainSize) == false) success = false;

   // Write ghost zone data (not applicable here):
   uint64_t dummy;
   if (vlsv.writeArray("MESH_GHOST_LOCALIDS",attributes,domainSize[1],1,&dummy) == false) success = false;
   if (vlsv.writeArray("MESH_GHOST_DOMAINS",attributes,domainSize[1],1,&dummy) == false) success = false;

   // Write node coordinates:
   vector<float> coords;
   coords.resize(bbox[0]*bbox[3]+1);
   float dx = (meshLimits[limits::XMAX]-meshLimits[limits::XMIN])/(bbox[0]*bbox[3]);
   for (uint32_t i=0; i<bbox[0]*bbox[3]+1; ++i) {
      coords[i] = meshLimits[0] + i*dx;
   }
   if (vlsv.writeArray("MESH_NODE_CRDS_X",attributes,coords.size(),1,&(coords[0])) == false) success = false;
   
   coords.resize(bbox[1]*bbox[4]+1);
   dx = (meshLimits[limits::YMAX]-meshLimits[limits::YMIN])/(bbox[1]*bbox[4]);
   for (uint32_t i=0; i<bbox[1]*bbox[4]+1; ++i) {
      coords[i] = meshLimits[2] + i*dx;
   }
   if (vlsv.writeArray("MESH_NODE_CRDS_Y",attributes,coords.size(),1,&(coords[0])) == false) success = false;
   
   coords.resize(bbox[2]*bbox[5]+1);
   dx = (meshLimits[limits::ZMAX]-meshLimits[limits::ZMIN])/(bbox[2]*bbox[5]);
   for (uint32_t i=0; i<bbox[2]*bbox[5]+1; ++i) {
      coords[i] = meshLimits[4] + i*dx;
   }
   if (vlsv.writeArray("MESH_NODE_CRDS_Z",attributes,coords.size(),1,&(coords[0])) == false) success = false;
   
   return success;
}

} // namespace amr
   
   