#include "../../inc/docker/docker.h"
#include "../../inc/parameters.h"
#include "../../inc/math/linalg.h"
#include <glm/gtx/string_cast.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <iostream>
#include <set>

Docker* Docker::docker_ptr = 0;

//------------------------------------------------------
//--------------------- INTERNAL -----------------------
//------------------------------------------------------
// This computes the geodesic distance between two patches
static int geodesic_distance(int lhs_patch_ind, int rhs_patch_ind, const SurfaceDescriptors& desc)
{
	const Patch& lhs_patch = desc[lhs_patch_ind].first;
	const Patch& rhs_patch = desc[rhs_patch_ind].first;

	//First version: just compute euclidean distance between patches centroid
	glm::dvec3 n1 = lhs_patch.get_pos();
	glm::dvec3 n2 = rhs_patch.get_pos();

	return glm::length(n1-n2);
}

// This compares two points in R³
static bool comp_point(const glm::dvec3& lhs, const glm::dvec3& rhs)
{
	if( lhs[0] != rhs[0] ) return lhs[0] < rhs[0];
	if( lhs[1] != rhs[1] ) return lhs[1] < rhs[1];
	if( lhs[2] != rhs[2] ) return lhs[2] < rhs[2];

	//lhs is equal to rhs, so is not less then rhs
	return false;
}

// This merges all patches of a group into a single cloud point
// and, at the same, computes the average normal of the cloud.
// 'though it's not nice to merge different operations in a single
// function, this is faster then computing separately.
static void build_cloud_from_group(const std::vector<int>& group,
									const SurfaceDescriptors& descriptors, 
									const Graph& target, 
									std::vector<glm::dvec3>& cloud_out,
									glm::dvec3& avg_normal)
{
	avg_normal = glm::dvec3(0.0, 0.0, 0.0);

	//we'll use a BST to store all points without repeating
	std::set<glm::dvec3,bool(*)(const glm::dvec3&,const glm::dvec3&)> cloud(comp_point);

	//we'll loop through all patches inside GROUP
	for(auto desc_pair = group.begin(); desc_pair != group.end(); ++desc_pair)
	{
		const Patch& patch = descriptors[*desc_pair].first;

		// accumulate normal
		avg_normal = avg_normal + patch.get_normal();

		// put every point inside this patch into the cloud.
		for(auto p = patch.nodes.begin(); p != patch.nodes.end(); ++p)
			cloud.insert( target.get_node(*p).get_pos() );
	}

	//copy set to vector
	cloud_out = std::vector<glm::dvec3>( cloud.begin(), cloud.end() );

	// compute average normal
	avg_normal = glm::normalize( avg_normal * (1.0 / group.size()) );
}

//-----------------------------------------------------------
//--------------------- FROM DOCKER.H -----------------------
//-----------------------------------------------------------
void Docker::build_matching_groups(const SurfaceDescriptors& desc_target, 
									const SurfaceDescriptors& desc_ligand, 
									std::vector<MatchingGroup>& groups_out) const
{
	//loop over all patches in target surface, get the
	//most similar and complementary patches from ligand,
	//then try to group it. 
	for(int t = 0; t < desc_target.size(); ++t)
	{
		const std::pair<Patch,Descriptor> &t_patch = desc_target[t];

		//This list stores pairs <D,I>, where I is the index of a
		//LIGAND patch and D is the dissimilary distance between
		//the current TARGET patch and I. It is inverted so we
		//can use lexicographical order for sorting
		std::vector< std::pair<double,int> > similarity_list;
		for(int l = 0; l < desc_ligand.size(); ++l)
		{	
			const std::pair<Patch,Descriptor> &l_patch = desc_ligand[l];
			
			if(t_patch.second.type != l_patch.second.type)
			{
				double dist = fabs(t_patch.second.curv - l_patch.second.curv) / std::max(t_patch.second.curv, l_patch.second.curv);
				similarity_list.push_back( std::make_pair(dist, l) );
			}
		}

		//sort similarity list according to distance. This will
		//give us a list of the patches in the LIGAND which are
		//most similar to t_patch (which is the current TARGET 
		//patch we're treating.
		std::sort(similarity_list.begin(), similarity_list.end());

		//get the K patches most similar to t_patch
		similarity_list.erase( similarity_list.begin() + Parameters::N_BEST_PAIRS, similarity_list.end() );

		//try to group pairs together
		for(auto lig = similarity_list.begin(); lig != similarity_list.end(); ++lig)
		{
			bool added = false;

			//build the current pair we're treating
			std::pair<int,int> cur_pair = std::make_pair(t, lig->second);

			for(auto grp = groups_out.begin(); grp != groups_out.end(); ++grp)
			{
				bool grouping_crit = true;

				//check distances
				for(auto pair = grp->begin(); pair != grp->end(); ++pair)
				{
					if( geodesic_distance(cur_pair.first, pair->first, desc_target) > Parameters::G_THRESH ) grouping_crit = false;
					if( geodesic_distance(cur_pair.second, pair->second, desc_ligand) > Parameters::G_THRESH ) grouping_crit = false;
				}

				//if group criterion holds, push cur_pair to group
				if(grouping_crit)
				{
					grp->push_back( cur_pair );
					added = true;
				}
			}

			//if cur_pair was not added to any group, create a new group
			if(!added)
			{
				MatchingGroup new_group;
				new_group.push_back( cur_pair );
				groups_out.push_back( new_group );
			}
		}
	}

	return;
}

// This function builds the transformations that aligns each of the
// matching groups. If we have X matching groups, we should X transformations in the end.
void Docker::transformations_from_matching_groups(const std::vector<MatchingGroup>& matching_groups, 
												const Graph& target, const SurfaceDescriptors& desc_target,
												const Graph& ligand, const SurfaceDescriptors& desc_ligand,
												std::vector<glm::dmat4>& mg_transformation) const
{
	// TODO: For future work, after translating and aligning we'll use
	// ICP with Regular Grid to get a better alignment for the clouds.
	
	for(auto MG = matching_groups.begin(); MG != matching_groups.end(); ++MG)
	{
		//0) Split matching group pairs into two vectors
		std::vector<int> target_groups, ligand_groups;
		for(auto p = MG->begin(); p != MG->end(); ++p)
		{
			target_groups.push_back( p->first );
			ligand_groups.push_back( p->second );
		}

		//1) Merge patches from TARGET group
		std::vector<glm::dvec3> target_cloud; glm::dvec3 target_normal;
		build_cloud_from_group(target_groups, desc_target, target, target_cloud, target_normal);

		//2) Merge patches from LIGAND group
		std::vector<glm::dvec3> ligand_cloud; glm::dvec3 ligand_normal;
		build_cloud_from_group(ligand_groups, desc_ligand, ligand, ligand_cloud, ligand_normal);

		//3) Compute centroids of each patch
		glm::dvec3 target_centroid = cloud_centroid(target_cloud);
		glm::dvec3 ligand_centroid = cloud_centroid(ligand_cloud);

		//4) Compute rotation that aligns the average normal of the patches.
		//	To accomplish this, the cross product between the two vectors gives us the
		//	axle of rotation and the dot product gives us the angle. We build
		//	a quaternion that rotates the first vector so to align it with the
		//	second one, then we get the 4x4 rotation matrix which is equivalent
		//	to this quaternion. Remember we need to rotate it around the centroid so not
		//  to translate it and change distances!
		//TODO: ROTATION IS NOT LINEAR IN THE END! DEFORMATION IS HAPPENING -> Rodrigues' formula?
		//TODO: Border cases: vectors form an angle of 0°, 180°?

		glm::dvec3 rot_axle = glm::cross(ligand_normal, target_normal);

		double rot_angle = (glm::acos(glm::dot(ligand_normal, target_normal)) + glm::pi<double>()) / 2.0;
		double rot_cos = glm::cos(rot_angle), rot_sin = glm::sin(rot_angle);

		glm::dquat quat_align_normals = glm::dquat(rot_cos,
												rot_axle.x * rot_sin, 
												rot_axle.y * rot_sin,
												rot_axle.z * rot_sin);

		glm::dmat4 align_normals = glm::mat4_cast(quat_align_normals);

		//5) Compose final transformation:
		//		send to origin, rotate, bring back to target centroid
		glm::dmat4 final_t = glm::translate(glm::dmat4(1.0), target_centroid) 
								* align_normals
								* glm::translate(glm::dmat4(1.0), -ligand_centroid);

		mg_transformation.push_back( final_t );
	}

	return;
}