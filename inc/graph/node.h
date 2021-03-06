#ifndef _NODE_H_
#define _NODE_H_

#include <vector>
#include <utility>
#include <glm/glm.hpp>
#include <string>
#include "convexity.h"

class Node
{
private:
	glm::dvec3 pos, curvature, normal;
	Convexity type;

	//useful for rendering only
	glm::vec3 color;

	//We store the indices to the neighbours
	std::vector<std::pair<int, int> > ngbr;

public:
	Node();
	Node(const glm::dvec3& pos, const glm::dvec3& normal);

	//-----------------------------------
	//--------- Access methods ----------
	//-----------------------------------
	glm::dvec3 get_curvature() const { return this->curvature; }
	glm::dvec3 get_pos() const { return this->pos; }
	glm::dvec3 get_normal() const { return this->normal; }
	glm::vec3 get_color() const { return this->color; }
	Convexity get_type() const { return this->type; }

	//TODO: change this for a function returning a const std::vector<>&
	int n_incident_faces() const { return ngbr.size(); }
	std::pair<int,int> get_face(int index) const;

	void set_curvature(const glm::dvec3& c);
	void set_convexity(const Convexity& type) { this->type = type; }
	void set_color(const glm::vec3& c) { this->color = c; }

	//-------------------------------
	//--------- Operations ----------
	//-------------------------------
	//These operations change the internal state of the node
	void push_triangular_face(int adj1, int adj2);
	void transform_node(const glm::dmat4& T);

	//--------------------------------
	//-------- Debugging ops ---------
	//--------------------------------
	std::string node2str() const;
};

#endif