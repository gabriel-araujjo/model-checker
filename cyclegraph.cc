#include "cyclegraph.h"
#include "action.h"
#include "common.h"
#include "promise.h"
#include "model.h"

/** Initializes a CycleGraph object. */
CycleGraph::CycleGraph() :
	discovered(new HashTable<const CycleNode *, const CycleNode *, uintptr_t, 4, model_malloc, model_calloc, model_free>(16)),
	hasCycles(false),
	oldCycles(false)
{
}

/** CycleGraph destructor */
CycleGraph::~CycleGraph()
{
	delete discovered;
}

/**
 * Add a CycleNode to the graph, corresponding to a store ModelAction
 * @param act The write action that should be added
 * @param node The CycleNode that corresponds to the store
 */
void CycleGraph::putNode(const ModelAction *act, CycleNode *node)
{
	actionToNode.put(act, node);
#if SUPPORT_MOD_ORDER_DUMP
	nodeList.push_back(node);
#endif
}

/**
 * @brief Returns the CycleNode corresponding to a given ModelAction
 * @param action The ModelAction to find a node for
 * @return The CycleNode paired with this action
 */
CycleNode * CycleGraph::getNode(const ModelAction *action)
{
	CycleNode *node = actionToNode.get(action);
	if (node == NULL) {
		node = new CycleNode(action);
		putNode(action, node);
	}
	return node;
}

/**
 * Adds an edge between two ModelActions. The ModelAction @a to is ordered
 * after the ModelAction @a from.
 * @param to The edge points to this ModelAction
 * @param from The edge comes from this ModelAction
 */
void CycleGraph::addEdge(const ModelAction *from, const ModelAction *to)
{
	ASSERT(from);
	ASSERT(to);

	CycleNode *fromnode = getNode(from);
	CycleNode *tonode = getNode(to);

	addEdge(fromnode, tonode);
}

/**
 * Adds an edge between two CycleNodes.
 * @param fromnode The edge comes from this CycleNode
 * @param tonode The edge points to this CycleNode
 */
void CycleGraph::addEdge(CycleNode *fromnode, CycleNode *tonode)
{
	if (!hasCycles)
		hasCycles = checkReachable(tonode, fromnode);

	if (fromnode->addEdge(tonode))
		rollbackvector.push_back(fromnode);

	/*
	 * If the fromnode has a rmwnode that is not the tonode, we should add
	 * an edge between its rmwnode and the tonode
	 */
	CycleNode *rmwnode = fromnode->getRMW();
	if (rmwnode && rmwnode != tonode) {
		if (!hasCycles)
			hasCycles = checkReachable(tonode, rmwnode);

		if (rmwnode->addEdge(tonode))
			rollbackvector.push_back(rmwnode);
	}
}

/**
 * @brief Add an edge between a write and the RMW which reads from it
 *
 * Handles special case of a RMW action, where the ModelAction rmw reads from
 * the ModelAction from. The key differences are:
 * (1) no write can occur in between the rmw and the from action.
 * (2) Only one RMW action can read from a given write.
 *
 * @param from The edge comes from this ModelAction
 * @param rmw The edge points to this ModelAction; this action must read from
 * ModelAction from
 */
void CycleGraph::addRMWEdge(const ModelAction *from, const ModelAction *rmw)
{
	ASSERT(from);
	ASSERT(rmw);

	CycleNode *fromnode = getNode(from);
	CycleNode *rmwnode = getNode(rmw);

	/* Two RMW actions cannot read from the same write. */
	if (fromnode->setRMW(rmwnode))
		hasCycles = true;
	else
		rmwrollbackvector.push_back(fromnode);

	/* Transfer all outgoing edges from the from node to the rmw node */
	/* This process should not add a cycle because either:
	 * (1) The rmw should not have any incoming edges yet if it is the
	 * new node or
	 * (2) the fromnode is the new node and therefore it should not
	 * have any outgoing edges.
	 */
	for (unsigned int i = 0; i < fromnode->getNumEdges(); i++) {
		CycleNode *tonode = fromnode->getEdge(i);
		if (tonode != rmwnode) {
			if (rmwnode->addEdge(tonode))
				rollbackvector.push_back(rmwnode);
		}
	}

	addEdge(fromnode, rmwnode);
}

#if SUPPORT_MOD_ORDER_DUMP
void CycleGraph::dumpNodes(FILE *file) const
{
	for (unsigned int i = 0; i < nodeList.size(); i++) {
		CycleNode *cn = nodeList[i];
		const ModelAction *action = cn->getAction();
		fprintf(file, "N%u [label=\"%u, T%u\"];\n", action->get_seq_number(), action->get_seq_number(), action->get_tid());
		if (cn->getRMW() != NULL) {
			fprintf(file, "N%u -> N%u[style=dotted];\n", action->get_seq_number(), cn->getRMW()->getAction()->get_seq_number());
		}
		for (unsigned int j = 0; j < cn->getNumEdges(); j++) {
			CycleNode *dst = cn->getEdge(j);
			const ModelAction *dstaction = dst->getAction();
			fprintf(file, "N%u -> N%u;\n", action->get_seq_number(), dstaction->get_seq_number());
		}
	}
}

void CycleGraph::dumpGraphToFile(const char *filename) const
{
	char buffer[200];
	sprintf(buffer, "%s.dot", filename);
	FILE *file = fopen(buffer, "w");
	fprintf(file, "digraph %s {\n", filename);
	dumpNodes(file);
	fprintf(file, "}\n");
	fclose(file);
}
#endif

/**
 * Checks whether one ModelAction can reach another.
 * @param from The ModelAction from which to begin exploration
 * @param to The ModelAction to reach
 * @return True, @a from can reach @a to; otherwise, false
 */
bool CycleGraph::checkReachable(const ModelAction *from, const ModelAction *to) const
{
	CycleNode *fromnode = actionToNode.get(from);
	CycleNode *tonode = actionToNode.get(to);

	if (!fromnode || !tonode)
		return false;

	return checkReachable(fromnode, tonode);
}

/**
 * Checks whether one CycleNode can reach another.
 * @param from The CycleNode from which to begin exploration
 * @param to The CycleNode to reach
 * @return True, @a from can reach @a to; otherwise, false
 */
bool CycleGraph::checkReachable(const CycleNode *from, const CycleNode *to) const
{
	std::vector< const CycleNode *, ModelAlloc<const CycleNode *> > queue;
	discovered->reset();

	queue.push_back(from);
	discovered->put(from, from);
	while (!queue.empty()) {
		const CycleNode *node = queue.back();
		queue.pop_back();
		if (node == to)
			return true;

		for (unsigned int i = 0; i < node->getNumEdges(); i++) {
			CycleNode *next = node->getEdge(i);
			if (!discovered->contains(next)) {
				discovered->put(next, next);
				queue.push_back(next);
			}
		}
	}
	return false;
}

bool CycleGraph::checkPromise(const ModelAction *fromact, Promise *promise) const
{
	std::vector< CycleNode *, ModelAlloc<CycleNode *> > queue;
	discovered->reset();
	CycleNode *from = actionToNode.get(fromact);

	queue.push_back(from);
	discovered->put(from, from);
	while (!queue.empty()) {
		CycleNode *node = queue.back();
		queue.pop_back();

		if (promise->eliminate_thread(node->getAction()->get_tid())) {
			return true;
		}

		for (unsigned int i = 0; i < node->getNumEdges(); i++) {
			CycleNode *next = node->getEdge(i);
			if (!discovered->contains(next)) {
				discovered->put(next, next);
				queue.push_back(next);
			}
		}
	}
	return false;
}

void CycleGraph::startChanges()
{
	ASSERT(rollbackvector.empty());
	ASSERT(rmwrollbackvector.empty());
	ASSERT(oldCycles == hasCycles);
}

/** Commit changes to the cyclegraph. */
void CycleGraph::commitChanges()
{
	rollbackvector.clear();
	rmwrollbackvector.clear();
	oldCycles = hasCycles;
}

/** Rollback changes to the previous commit. */
void CycleGraph::rollbackChanges()
{
	for (unsigned int i = 0; i < rollbackvector.size(); i++)
		rollbackvector[i]->popEdge();

	for (unsigned int i = 0; i < rmwrollbackvector.size(); i++)
		rmwrollbackvector[i]->clearRMW();

	hasCycles = oldCycles;
	rollbackvector.clear();
	rmwrollbackvector.clear();
}

/** @returns whether a CycleGraph contains cycles. */
bool CycleGraph::checkForCycles() const
{
	return hasCycles;
}

/**
 * @brief Constructor for a CycleNode
 * @param act The ModelAction for this node
 */
CycleNode::CycleNode(const ModelAction *act) :
	action(act),
	promise(NULL),
	hasRMW(NULL)
{
}

/**
 * @brief Constructor for a Promise CycleNode
 * @param promise The Promise which was generated
 */
CycleNode::CycleNode(const Promise *promise) :
	action(NULL),
	promise(promise),
	hasRMW(NULL)
{
}

/**
 * @param i The index of the edge to return
 * @returns The a CycleNode edge indexed by i
 */
CycleNode * CycleNode::getEdge(unsigned int i) const
{
	return edges[i];
}

/** @returns The number of edges leaving this CycleNode */
unsigned int CycleNode::getNumEdges() const
{
	return edges.size();
}

CycleNode * CycleNode::getBackEdge(unsigned int i) const
{
	return back_edges[i];
}

unsigned int CycleNode::getNumBackEdges() const
{
	return back_edges.size();
}

/**
 * @brief Remove an element from a vector
 * @param v The vector
 * @param n The element to remove
 * @return True if the element was found; false otherwise
 */
template <typename T>
static bool vector_remove_node(std::vector<T, SnapshotAlloc<T> >& v, const T n)
{
	for (unsigned int i = 0; i < v.size(); i++) {
		if (v[i] == n) {
			v.erase(v.begin() + i);
			return true;
		}
	}
	return false;
}

/**
 * @brief Remove a (forward) edge from this CycleNode
 * @return The CycleNode which was popped, if one exists; otherwise NULL
 */
CycleNode * CycleNode::removeEdge()
{
	if (edges.empty())
		return NULL;

	CycleNode *ret = edges.back();
	edges.pop_back();
	vector_remove_node(ret->back_edges, this);
	return ret;
}

/**
 * @brief Remove a (back) edge from this CycleNode
 * @return The CycleNode which was popped, if one exists; otherwise NULL
 */
CycleNode * CycleNode::removeBackEdge()
{
	if (back_edges.empty())
		return NULL;

	CycleNode *ret = back_edges.back();
	back_edges.pop_back();
	vector_remove_node(ret->edges, this);
	return ret;
}

/**
 * Adds an edge from this CycleNode to another CycleNode.
 * @param node The node to which we add a directed edge
 * @return True if this edge is a new edge; false otherwise
 */
bool CycleNode::addEdge(CycleNode *node)
{
	for (unsigned int i = 0; i < edges.size(); i++)
		if (edges[i] == node)
			return false;
	edges.push_back(node);
	node->back_edges.push_back(this);
	return true;
}

/** @returns the RMW CycleNode that reads from the current CycleNode */
CycleNode * CycleNode::getRMW() const
{
	return hasRMW;
}

/**
 * Set a RMW action node that reads from the current CycleNode.
 * @param node The RMW that reads from the current node
 * @return True, if this node already was read by another RMW; false otherwise
 * @see CycleGraph::addRMWEdge
 */
bool CycleNode::setRMW(CycleNode *node)
{
	if (hasRMW != NULL)
		return true;
	hasRMW = node;
	return false;
}
