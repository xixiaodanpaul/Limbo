/*************************************************************************
    > File Name: LpDualMcf.h
    > Author: Yibo Lin
    > Mail: yibolin@utexas.edu
    > Created Time: Mon 13 Oct 2014 07:30:40 PM CDT
 ************************************************************************/

#ifndef _LPMCF_LPDUALMCF_H
#define _LPMCF_LPDUALMCF_H

#include <cstdlib>
#include <iostream>
#include <cassert>
#include <string>
#include <cctype>
#include <ctime>
#include <boost/cstdint.hpp>
#include <boost/unordered_map.hpp>
#include <boost/foreach.hpp>
#include <boost/typeof/typeof.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <limbo/solvers/lpmcf/Lgf.h>
#include <limbo/parsers/lp/bison/LpDriver.h>

using std::cout;
using std::endl;
using std::string;
using std::pair;
using std::make_pair;
using boost::int32_t;
using boost::int64_t;
using boost::unordered_map;
using boost::iequals;

// solving LP with min-cost flow 
namespace LpMcf 
{
// hash calculator for pairs 
template <typename T1, typename T2>
struct hash_pair : pair<T1, T2>
{
	typedef pair<T1, T2> base_type;
	using typename base_type::first_type;
	using typename base_type::second_type;

	hash_pair() : base_type() {}
	hash_pair(first_type const& a, second_type const& b) : base_type(a, b) {}
	hash_pair(base_type const& rhs) : base_type(rhs) {}

	bool operator==(base_type const& rhs) const 
	{return this->first == rhs.first && this->second == rhs.second;}

	friend std::size_t hash_value(base_type const& key) 
	{
		std::size_t seed = 0;
		boost::hash_combine(seed, key.first);
		boost::hash_combine(seed, key.second);
		return seed;
	}
};

// LP solved with min-cost flow 
// the dual problem of this LP is a min-cost flow problem 
// so we can solve the graph problem and then 
// call shortest path algrithm to calculate optimum of primal problem 
//
// 1. primal problem 
// min. sum ci*xi
// s.t. xi - xj >= bij for (i, j) in E
//      di <= xi <= ui for i in [1, n]
//
// 2. introduce new variables yi in [0, n]
//    set xi = yi - y0 
// min. sum ci*(yi-y0)
// s.t. yi - yj >= bij for (i, j) in E 
//      di <= yi - y0 <= ui for i in [1, n]
//      yi is unbounded integer for i in [0, n]
//
// 3. re-write the problem 
//                              ci for i in [1, n]
// min. sum ci*yi, where ci =   - sum ci for i in [1, n]
//                    bij for (i, j) in E 
// s.t. yi - yj >=    di  for j = 0, i in [1, n]
//                    -ui for i = 0, i in [1, n]
//      yi is unbounded integer for i in [0, n]
//
// 4. map to dual min-cost flow problem 
//    let's use c'i for generalized ci and b'ij for generalized bij 
//    c'i is node supply 
//    for each (i, j) in E', an arc from i to j with cost -b'ij and flow range [0, unlimited]
template <typename T = int64_t>
class LpDualMcf : public Lgf<T>, public LpParser::LpDataBase
{
	public:
		// value_type can only be integer types 
		typedef T value_type;
		typedef Lgf<value_type> base_type1;
		typedef LpParser::LpDataBase base_type2;
		using typename base_type1::cost_type;
		using typename base_type1::graph_type;
		using typename base_type1::node_type;
		using typename base_type1::arc_type;
		using typename base_type1::node_value_map_type;
		using typename base_type1::node_name_map_type;
		using typename base_type1::arc_value_map_type;
		using typename base_type1::arc_cost_map_type;
		using typename base_type1::arc_flow_map_type;
		using typename base_type1::node_pot_map_type;

		// I don't know why it does not work with 
		// using typename base_type1::alg_type;
		typedef typename base_type1::alg_type alg_type;

		// standard format 
		// li <= xi <= ui 
		// mapping to 
		// node i 
		// arcs from node i to node st 
		struct variable_type 
		{
			string name;
			pair<value_type, value_type> range;
			value_type weight;
			value_type value;
			node_type node;

			variable_type(string const& n, 
					value_type const& l = 0, 
					value_type const& r = std::numeric_limits<value_type>::max(),
					value_type const& w = 0, 
					value_type const& v = 0)
				: name(n), range(make_pair(l, r)), weight(w), value(v) {}

			bool is_lower_bounded() const {return range.first != std::numeric_limits<value_type>::min();}
			bool is_upper_bounded() const {return range.second != std::numeric_limits<value_type>::max();}
			bool is_bounded() const {return is_lower_bounded() || is_upper_bounded();}
		};

		// standard format 
		// xi - xj >= cij 
		// mapping to 
		// xi ----> xj, cost = -cij
		struct constraint_type 
		{
			pair<string, string> variable;
			value_type constant;
			arc_type arc;
			
			constraint_type(string const& xi, string const& xj, value_type const& c)
				: variable(make_pair(xi, xj)), constant(c) {}
		};

		LpDualMcf() 
			: base_type1(), 
			base_type2(),
			m_is_bounded(false), 
			m_M(std::numeric_limits<value_type>::max() >> 2) // use as unlimited number 
		{
		}

		// add variable with range 
		// default range is 
		// -inf <= xi <= inf 
		virtual void add_variable(string const& xi, 
				value_type const& l = std::numeric_limits<value_type>::min(), 
				value_type const& r = std::numeric_limits<value_type>::max())
		{
			// no variables with the same name is allowed 
			BOOST_AUTO(found, m_hVariable.find(xi));
			if (found == m_hVariable.end())
				assert(m_hVariable.insert(make_pair(xi, variable_type(xi, l, r, 0))).second);
			else 
			{
				found->second.range.first = std::max(found->second.range.first, l);
				found->second.range.second = std::min(found->second.range.second, r);

				// if user set bounds to variables 
				// switch to bounded mode, which means there will be an additional node to the graph 
				if (found->second.is_bounded())
					this->is_bounded(true);
			}
		}
		// add constraint 
		// xi - xj >= cij 
		// assume there's no duplicate 
		virtual void add_constraint(string const& xi, string const& xj, cost_type const& cij)
		{
			BOOST_AUTO(found, m_hConstraint.find(make_pair(xi, xj)));
			if (found == m_hConstraint.end())
				assert(m_hConstraint.insert(
							make_pair(
								make_pair(xi, xj), 
								constraint_type(xi, xj, cij)
								)
							).second
						);
			else // automatically reduce constraints 
				found->second.constant = std::max(found->second.constant, cij);
		}
		// add linear terms for objective function of LP 
		// we allow repeat adding weight 
		virtual void add_objective(string const& xi, value_type const& w)
		{
			if (w == 0) return;

			BOOST_AUTO(found, m_hVariable.find(xi));
			assert(found != m_hVariable.end());

			found->second.weight += w;
		}

		// check if lp problem is bounded 
		bool is_bounded() const {return m_is_bounded;}
		void is_bounded(bool v) {m_is_bounded = v;}

		// read lp format 
		// and then dump solution 
		void operator()(string const& filename)
		{
			read_lp(filename);
			(*this)();
			this->print_solution(filename+".sol");
		}
		// execute solver 
		// write out solution file if provided 
		void operator()() 
		{
			prepare();
#ifdef DEBUG 
			this->print_graph();
#endif 
			run();
		}
		// return solution 
		value_type solution(string const& xi) const 
		{
			BOOST_AUTO(found, m_hVariable.find(xi));
			assert(found != m_hVariable.end());

			return found->second.value;
		}
		// read lp format 
		// initializing graph 
		void read_lp(string const& filename) 
		{
			LpParser::read(*this, filename);
		}

		virtual void print_solution(string const& filename) const 
		{
			this->base_type1::print_solution(filename);

			std::ofstream out (filename.c_str(), std::ios::app);
			if (!out.good())
			{
				cout << "failed to open " << filename << endl;
				return;
			}

			out << "############# LP Solution #############" << endl;
			for (BOOST_AUTO(it, this->m_hVariable.begin()); it != this->m_hVariable.end(); ++it)
			{
				variable_type const& variable = it->second;
				out << this->m_hName[variable.node] << ": " << variable.value << endl;
			}

			out.close();
		}
	protected:
		// prepare before run 
		void prepare()
		{
			// 1. preparing nodes 
			// set supply to its weight in the objective 
			for (BOOST_AUTO(it, m_hVariable.begin()); it != m_hVariable.end(); ++it)
			{
				variable_type& variable = it->second;
				node_type const& node = this->m_graph.addNode();
				variable.node = node;
				this->m_hSupply[node] = variable.weight; 
				this->m_hName[node] = variable.name;
			}

			// 2. preparing arcs 
			// arcs constraints like xi - xj >= cij 
			// add arc from node i to node j with cost -cij and capacity unlimited 
			for (BOOST_AUTO(it, m_hConstraint.begin()); it != m_hConstraint.end(); ++it)
			{
				constraint_type& constraint = it->second;
				BOOST_AUTO(found1, m_hVariable.find(constraint.variable.first));
				BOOST_AUTO(found2, m_hVariable.find(constraint.variable.second));
				assert(found1 != m_hVariable.end()); 
				assert(found2 != m_hVariable.end()); 

				variable_type const& variable1 = found1->second;
				variable_type const& variable2 = found2->second;

				node_type const& node1 = variable1.node;
				node_type const& node2 = variable2.node;

				arc_type const& arc = this->m_graph.addArc(node1, node2);
				constraint.arc = arc;

				this->m_hCost[arc] = -constraint.constant;
				this->m_hLower[arc] = 0;
				//m_hUpper[arc] = std::numeric_limits<value_type>::max();
				this->m_hUpper[arc] = m_M;
			}
			// 3. arcs for variable bounds 
			// from node to additional node  
			if (this->is_bounded())
			{
				// 3.1 create additional node 
				// its corresponding weight is the negative sum of weight for other nodes 
				m_addl_node = this->m_graph.addNode();
				value_type addl_weight = 0;
				for (BOOST_AUTO(it, m_hVariable.begin()); it != m_hVariable.end(); ++it)
					addl_weight -= it->second.weight;
				this->m_hSupply[m_addl_node] = addl_weight; 
				this->m_hName[m_addl_node] = "lpmcf_additional_node";

				for (BOOST_AUTO(it, m_hVariable.begin()); it != m_hVariable.end(); ++it)
				{
					variable_type const& variable = it->second;
					// has lower bound 
					// add arc from node to additional node with cost d and cap unlimited
					if (variable.is_lower_bounded())
					{
						arc_type const& arc = this->m_graph.addArc(variable.node, m_addl_node);
						this->m_hCost[arc] = -variable.range.first;
						this->m_hLower[arc] = 0;
						this->m_hUpper[arc] = m_M;
					}
					// has upper bound 
					// add arc from additional node to node with cost u and capacity unlimited
					if (variable.is_upper_bounded())
					{
						arc_type const& arc = this->m_graph.addArc(m_addl_node, variable.node);
						this->m_hCost[arc] = variable.range.second;
						this->m_hLower[arc] = 0;
						this->m_hUpper[arc] = m_M;
					}
				}
			}
		}
		typename alg_type::ProblemType run()
		{
			// 1. choose algorithm 
			alg_type alg (this->m_graph);

			// 2. run 
			typename alg_type::ProblemType status = alg.reset().resetParams()
				.lowerMap(this->m_hLower)
				.upperMap(this->m_hUpper)
				.costMap(this->m_hCost)
				.supplyMap(this->m_hSupply)
				.run();

			// 3. check results 
#ifdef DEBUG
			switch (status)
			{
				case alg_type::OPTIMAL:
					cout << "OPTIMAL" << endl;
					break;
				case alg_type::INFEASIBLE:
					cout << "INFEASIBLE" << endl;
					break;
				case alg_type::UNBOUNDED:
					cout << "UNBOUNDED" << endl;
					break;
			}

			assert(status == alg_type::OPTIMAL);
#endif 
			// 4. update solution 
			if (status == alg_type::OPTIMAL)
			{
				this->m_totalcost = alg.template totalCost<cost_type>();
				// get dual solution of LP, which is the flow of mcf, skip this if not necessary
				alg.flowMap(this->m_hFlow);
				// get solution of LP, which is the dual solution of mcf 
				alg.potentialMap(this->m_hPot);

				// update solution 
				value_type addl_value = 0;
				if (this->is_bounded())
					addl_value = this->m_hPot[m_addl_node];
				for (BOOST_AUTO(it, m_hVariable.begin()); it != m_hVariable.end(); ++it)
				{
					variable_type& variable = it->second;
					variable.value = this->m_hPot[variable.node]-addl_value;
				}
			}

			return status;
		}

		bool m_is_bounded; // whether the problem is bounded or not 
		node_type m_addl_node; // additional node, only valid when m_is_bounded = true  

		value_type m_M; // a very large number to deal with ranges of variables 
						// reference from MIT paper: solving the convex cost integer dual network flow problem 
		// variable map 
		unordered_map<string, variable_type> m_hVariable;
		// constraint map 
		unordered_map<hash_pair<string, string>, constraint_type> m_hConstraint;
};

} // namespace LpMinCostFlow

#endif 