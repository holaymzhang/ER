"""
How to reload this module:
    import hase
    hase.foo()
    # modify source code
    reload(hase)
    # hase should be reloaded now
    hase.foo()

Some Initialization process was copied from the preload.py (of scripting).
Nodes should already have an integer attr named "idepi" representing indirect depth.
    e.g., for v in g.nodes: v.idepi = int(v.idep)
"""
# initialize
try:
    import org.openide.util.Lookup as Lookup
    import org.gephi.layout.api.LayoutController
    LayoutController = Lookup.getDefault().lookup(org.gephi.layout.api.LayoutController)
    import org.gephi.visualization.VizController
    VizController = Lookup.getDefault().lookup(org.gephi.visualization.VizController)
    import org.gephi.layout.plugin.forceAtlas2.ForceAtlas2Builder as ForceAtlas2
    import java.awt.Color as jcolor
    import time
    import org.gephi.scripting.wrappers.GyNode as GyNode
    import org.gephi.scripting.wrappers.GyGraph as GyGraph
except ImportError:
    print("Failed to import Gephi related lib, fallback to cli mode")
    import sys
    import json
    from fakegynode import FakeGyNode
    from fakegynode import FakeGyEdge

def RunForceAtlas2_nooverlap(iters):
    fa2 = ForceAtlas2().buildLayout()
    LayoutController.setLayout(fa2)
    fa2.setScalingRatio(2.0)
    fa2.setGravity(-2)
    fa2.setAdjustSizes(0)
    LayoutController.executeLayout(iters)
    while (LayoutController.getModel().isRunning()):
        time.sleep(0.2)
    print("%d iters with False AdjustSizes done" % (iters))
    LayoutController.setLayout(fa2)
    fa2.setScalingRatio(2.0)
    fa2.setGravity(-2)
    fa2.setAdjustSizes(1)
    LayoutController.executeLayout(200)
    while (LayoutController.getModel().isRunning()):
        time.sleep(0.2)
    print(" done")

"""
Represent a recordable instruction (valid kinst) in the expression/constraint
graph

@type kinst: str
@param kinst: a global unique identifier of this instruction

@type width:int
@param width: the width of the result(destination register) of this instruction

@type freq: int
@param freq: how many times this instruction got executed in the entire trace

@type max_idep: int
@param max_idep: the maximum indirect depth of nodes which still cannot be
concretized

@type rec_nodes: set(node.id)
@param rec_nodes: what symbolic nodes are directly recorded if you record this instruction

@type hidden_nodes: set(node.id)
@param hidden_nodes: what symbolic nodes related to other instruction but still can be
concretized if you record this instruction

@type concretized_nodes: set(node.id)
@param concretized_nodes: what symbolic nodes (not ConstantExpr) can be
concretized after you record this instruction. Note that this list will include
rec_nodes and hidden_nodes, but will not include any ConstantExpr nodes.

A RecordableInstruction will be represented by a list of RecordableNode
"""
class RecordableInst(object):
    def __init__(self, kinst, width, freq, rec_nodes, hidden_nodes, concretized_nodes):
        self.kinst = kinst
        self.width = width
        self.freq = freq
        self.rec_nodes = rec_nodes
        self.hidden_nodes = hidden_nodes
        self.concretized_nodes = concretized_nodes
        # sanity check
        if not isinstance(rec_nodes, set):
            raise RuntimeError("rec_nodes is not a set ")
        if not isinstance(hidden_nodes, set):
            raise RuntimeError("hidden_nodes is not a set ")
        if not isinstance(concretized_nodes, set):
            raise RuntimeError("concretized_nodes is not a set ")
        if width == 0:
            raise RuntimeError("Zero Width instruction")

    def __str__(self):
        return "kinst: %s, width: %d, freq: %d, %d nodes recorded, %d nodes hidden,"\
                "%d nodes concretized" % (self.kinst, self.width, self.freq,\
                len(self.rec_nodes), len(self.hidden_nodes),
                len(self.concretized_nodes))

    def __repr__(self):
        return self.__str__()

    """
    Create easy to check dict
    @type recinsts: List(RecordableInst)
    @param recinsts: a list of RecordableInstructions

    @rtype Dict(str->RecordableInst)
    @return: a dict maps kinst string to associated RecordableInst object
    """
    @staticmethod
    def getStrDict(recinsts):
        d = {}
        for r in recinsts:
            d[r.kinst] = r
        return d
"""
Check if the kinst of a GyNode is valid
node(GyNode)
"""
def isKInstValid(node):
    return (node.kinst is not None) and \
            (len(node.kinst) > 0) and \
            node.kinst != 'N/A'

"""
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!IMPORTANT!!!!!!!!!!!!!!!!!!!!!!!!!!!
!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
The GyNode type (python wrapper of a Java object) is buggy in terms of Set
operations.
E.g. Bellow python code segment will give you "False"
```
l = [ x for x in g.nodes ]
l[0] in g.nodes
```
"""
class PyGraph(object):

    """
    @type gygraph: GyGraph
    @param gygraph: the origin graph available in the python script plugin ('g')

    @rtype: PyGraph
    @return: A new PyGraph built from the given GyGraph
    """
    @classmethod
    def buildFromGyGraph(cls, gygraph):
        if isinstance(gygraph, GyGraph):
            # Here we filter out the "dummy nodes" which are used to scale edge
            # width in Gephi
            gynodes = set([n for n in gygraph.nodes if n.kind is not None])
            gyedges = set([e for e in gygraph.edges if e.source.kind is not None
                and e.target.kind is not None])
            return PyGraph(gynodes, gyedges)
        else:
            return None

    """
    @type pygraph: PyGraph
    @param pygraph: the base graph you are working on
    @type deleted_nodes: set(node_id)
    @param deleted_nodes: a set of node_id you want to delete from pygraph
    """
    @classmethod
    def buildFromPyGraph(cls, pygraph, deleted_nodes):
        if isinstance(pygraph, PyGraph) and isinstance(deleted_nodes, set):
            subgynodes = set([ n for n in pygraph.gynodes if n.id not in
                deleted_nodes])
            subgyedges = set([e for e in pygraph.gyedges if e.source.id not in
                deleted_nodes and e.target.id not in deleted_nodes])
            return PyGraph(subgynodes, subgyedges)
        else:
            return None

    """
    @type graphdict: Dict, The json graph seems like:
    {
        "edges": [
            {
                "source": "94530732880448",
                "target": "94530732878272",
                "weight": 1.0
            }, ...
        ]
        "nodes": {
            "94530648900736": {
                "Category": "N"/"Q"/"C",
                "DbgInfo": "N/A",
                "Freq": 0,
                "IDep": 0,
                "KInst": "N/A",
                "Kind": 3,
                "Width": 8,
                "label": "model_version[0]"
            }, ...
        }
    }
    """
    @classmethod
    def buildFromPyDict(cls, graphdict):
        nodes = graphdict["nodes"]
        edges = graphdict["edges"]
        fakegynodes = {}
        for nid, nprop in nodes.items():
            fakegynodes[nid] = FakeGyNode(nid, nprop)
        GyNodeSet = set(fakegynodes.values())
        GyEdgeSet = set([FakeGyEdge(fakegynodes[e["source"]],
        fakegynodes[e["target"]], e) for e in edges])
        return PyGraph(GyNodeSet, GyEdgeSet)

    """
    @type GyNodeSet: set(GyNode)
    @param GyNodeSet: set of GyNodes you want build graph from
    @type GyEdgeSet: set(GyEdge)
    @param GyEdgeSet: set of GyEdges you want to build graph from
    """
    def __init__(self, GyNodeSet, GyEdgeSet):
        self.gynodes = GyNodeSet
        self.gyedges = GyEdgeSet

        # edge map node.id -> set of edges starting from node
        self.edges = {}
        # reverse edge map, node.id -> set of edges ending in node
        self.redges = {}
        # map node.id -> node
        self.id_map = {}
        # node.id -> topological id (0..|V|-1)
        # note that the klee expression graph looks like
        # [operator] -> [operand0]
        #            -> [operand1]
        # so the edge represents [dependant] -> [dependency]
        # The assigned topological id: [dependant] > [dependency]
        # aka [result] > [operands]
        self.topological_map = None
        # @type: List(GyNode)
        # list of nodes from small topo id to large topo id
        # (from high indirect depth to low indirect depth)
        self.all_nodes_topo_order = None

        # @type: Dict(str->set(GyNode))
        self.kinst2nodes = None
        # @type: Dict(node_id->int)
        # map a node_id to indirect depth
        self.idep_map = None

        # innodes: no in edges, outnodes: no out edges
        # @type: set(node_id)
        self.innodes = set()
        self.outnodes = set()
        for e in self.gyedges:
            self.edges.setdefault(e.source.id, set()).add(e)
            self.redges.setdefault(e.target.id, set()).add(e)
        for n in self.gynodes:
            self.id_map[n.id] = n
            if n.id not in self.redges:
                self.innodes.add(n.id)
            if n.id not in self.edges:
                self.outnodes.add(n.id)
        self.topological_sort()
        self.build_kinst2nodes()
        self.all_nodes_topo_order = sorted(self.gynodes,
                key=lambda n: self.topological_map[n.id])
        self.calculate_idep()

    """
    Perform topological sort and store the result in self.topological_map
    """
    def topological_sort(self):
        topological_cnt = 0
        self.topological_map = {}
        # set of node.id
        visited_nodes = set()
        # list of node
        worklist = []
        for node in self.gynodes:
            if node.id not in visited_nodes:
                worklist.append(node)
                while len(worklist) > 0:
                    n = worklist[-1]
                    if n.id not in visited_nodes:
                        visited_nodes.add(n.id)
                        for e in self.edges.get(n.id, []):
                            if e.target.id not in visited_nodes:
                                worklist.append(e.target)
                    else:
                        if n.id not in self.topological_map:
                            self.topological_map[n.id] = topological_cnt
                            topological_cnt = topological_cnt + 1
                        worklist.pop()

    """
    Dependency: all_nodes_topo_order
    calculate indirect depth of all nodes.
    Will traverse nodes in the reverse topological order.
    @rtype: None
    """
    def calculate_idep(self):
        self.idep_map = {}
        for node in reversed(self.all_nodes_topo_order):
            if node.id not in self.redges:
                self.idep_map[node.id] = 0
            else:
                possible_idep = []
                for e in self.redges[node.id]:
                    parent_idep = self.idep_map[e.source.id]
                    if e.weight == 1.0:
                        possible_idep.append(parent_idep)
                    elif e.weight == 1.5:
                        possible_idep.append(parent_idep + 1)
                    else:
                        print("edge: " + e + " has invalid weight")
                        raise RuntimeError("Invalid edge weight")
                self.idep_map[node.id] = max(possible_idep)

    """
    Dependency: calculate_idep
    @rtype: int
    @return: the max indirect depth in the current graph
    """
    def max_idep(self):
        return max(self.idep_map.values())

    """
    build the mapping from recordable instructions to all associated nodes in
    the constraint graph
    """
    def build_kinst2nodes(self):
        self.kinst2nodes = {}
        for node in self.gynodes:
            if isKInstValid(node):
                self.kinst2nodes.setdefault(node.kinst, set()).add(node.id)


    """
    Dependency: topological_map
    Dependency: kinst2nodes
    Analyze all recordable nodes on the constraint graph
    Optimization:
    Given two recordable nodes n1 and n2, if only concretize n1 will also
    concretize n2, then n2 will be considered unnecessary to record and only n1
    will be reported (if no other recordable nodes can concretize n1).
    @type recinsts: List(RecordableInst)
    @param recinsts: list of recordable instructions we already decided to
    record

    @rtype: List(List(RecordableInst))
    @return: a list all beneficial recordable instructions, each entry is a list
    of interesting recordable instructions accumulated so far.
    note that each outer list entry represents a sequence of accumulated
    recordable instructions. And each inner list entry represent a recordable
    instruction. All outer list entries are lists of the same length x. And the
    first x-1 inner list entries of each outer list entries are same. Only the
    last inner list entry (newly added interesting RecordableInst) differs,
    which we should sort as key.
    instruction)
    """
    def analyze_recordable(self, recinsts=[]):
        # checked_kinst_set contains nodes no long require analysis
        # @type Set(node.id)
        checked_kinst_set = set()

        # concretized_set contains all nodes concretized by either ConstantExpr
        # or the already recorded instructions
        # @type Set(node.id)
        concretized_set = set()
        # populate data structures using input recinsts
        for recinst in recinsts:
            for nid in recinst.rec_nodes:
                concretized_set.add(nid)
                checked_kinst_set.add(nid)
            for nid in recinst.hidden_nodes:
                checked_kinst_set.add(nid)

        # @type: List(List(RecordableInst))
        result = []
        # Pre Process
        for node in self.all_nodes_topo_order:
            # find the closure of given concretized_set
            if node.id in self.edges and \
               all([(e.target.kind == "0") or (e.target.id in concretized_set) \
                    for e in self.edges[node.id]]):
                concretized_set.add(node.id)
        # union the sets of concretized nodes from multiple recordable
        # instructions
        in_all_concretized_nodes = set()
        for recinst in recinsts:
            for nid in recinst.concretized_nodes:
                in_all_concretized_nodes.add(nid)

        if concretized_set != in_all_concretized_nodes:
            print("Warn: input graph is not simplified, "
                  "dangling constant nodes detected")

        for seqid, n in enumerate(self.all_nodes_topo_order):
            if (isKInstValid(n)) and (n.id not in checked_kinst_set):
                local_concretized_set = concretized_set.copy()
                hidden_nodes = set()
                max_unconcretized_depth = 0
                for nid in self.kinst2nodes[n.kinst]:
                    checked_kinst_set.add(nid)
                    local_concretized_set.add(nid)
                for node in self.all_nodes_topo_order[seqid+1:]:
                    # skip ConstantExpr and nodes without out edges
                    # only consider nontrivial intermediate nodes
                    if (node.kind != "0") and (node.id in self.edges) and \
                    (node.id not in local_concretized_set):
                        const_nodes = [e.target.id for e in self.edges[node.id]
                                if e.target.kind == "0"]
                        known_symbolic_nodes = [e.target.id for e in
                                self.edges[node.id] if e.target.id in
                                local_concretized_set]
                        # this node can be concretized
                        if len(const_nodes) + len(known_symbolic_nodes) == \
                        len(self.edges[node.id]):
                            local_concretized_set.add(node.id)
                            # this node is hidden if:
                            # 1) it can be concretized here
                            # 2) it has a valid KInst
                            # 3) (TODO) recording this node is guaranteed to
                            # record more data than recording the recordable
                            # instruction we are currently looking at.
                            if len(known_symbolic_nodes) > 0 and isKInstValid(node):
                                checked_kinst_set.add(node.id)
                                hidden_nodes.add(node.id)
                        if len(const_nodes) + len(known_symbolic_nodes) > \
                        len(self.edges[node.id]):
                            raise RuntimeError("sum of out edges wrong")
                result.append(recinsts + [RecordableInst(n.kinst, int(n.width),
                    int(n.freq), self.kinst2nodes[n.kinst], hidden_nodes,
                    local_concretized_set - concretized_set)])
        return result

    """
    @rtype: float
    @return: the heuristic score of this Recordable Instruction. expression
    width and indirect depth are considered
    """
    def coverageScore(self, recinsts):
        return float(sum([int(self.id_map[nid].width)*int(self.id_map[nid].idep) \
                for recinst in recinsts \
                for nid in recinst.concretized_nodes]))
    """
    @type recinstsL: List(List(RecordableInst))
    @param recinstsL: list of list of recordable instructions from
    analyze_recordable()

    @rtype: List(List(RecordableInst))
    @return: sorted list, sorted according to coverageScore
    """
    def sortRecInstsbyCoverageScore(self, recinstsL):
        return sorted(recinstsL, key=lambda recinsts:
                self.coverageScore(recinsts))

    """
    @rtype: float
    @return: Heuristics take instruction frequency into consideration
    """
    def coverageScoreFreq(self, recinsts):
        # since we cannot just record 8B in a ptwrite instruction
        # we have to assume each recordable instructions cost 64B
        # so we use 64B instead of recinst.width
        return self.coverageScore(recinsts) /\
               (sum([recinst.freq for recinst in recinsts]) * 64)

    """
    same as above
    """
    def sortRecInstsbyCoverageScoreFreq(self, recinstsL):
        return sorted(recinstsL, key=lambda recinsts:
                self.coverageScoreFreq(recinsts))

    """
    @type recinsts: List(RecordableInst)
    @param recinsts: list of recordable instructions selected in one or more
    iterations to record. These instructions will be considered as one entity.

    @rtype: str
    @return: A string represents important information of recorded nodes in a
    RecordableInst
    """
    def getRecInstsInfo(self, recinsts):
        concretized_nodes = set()
        max_unconcretized_depth = 0
        for recinst in recinsts:
            if concretized_nodes & recinst.concretized_nodes:
                raise RuntimeError(
                "recordable instruction list has concretized_nodes overlap")
            concretized_nodes |=  recinst.concretized_nodes
        # the subgraph after deleting all concretized nodes
        subgraph = self.buildFromPyGraph(self, concretized_nodes)
        msgstring = ""
        for seq, recinst in enumerate(recinsts):
            msgstring += "Rec[%d]: " % seq
            msgstring += recinst.__str__() + "\n"
            msgstring += 'rec_nodes_label: ' + \
                ', '.join([self.id_map[nid].label for nid in recinst.rec_nodes])
            msgstring += "\n"

        msgstring += "CoverageScore=%f, CoverageFreqScore=%f\n" % (
                self.coverageScore(recinsts), self.coverageScoreFreq(recinsts))
        msgstring += "Total: "
        percent_concretized = \
        (float(len(concretized_nodes))/len(self.gynodes)*100)
        msgstring += '%d(%f%%) nodes concretized, max idep %d.' % \
                (len(concretized_nodes), percent_concretized,\
                        subgraph.max_idep())
        return msgstring

    """
    Print all recordable instruction candiadates in the given order
    @type recinstsL: List(List(RecordableInst))
    @param recinstsL: list of list of recordable instructions from
    analyze_recordable() or sorted by some heuristics.

    @rtype: None
    """
    def printCandidateRecInstsInfo(self, recinstsL):
        for seq, recinsts in enumerate(recinstsL):
            print(("###(%4d)###\n" % seq) + self.getRecInstsInfo(recinsts) + '\n')

    def ColorCSet(self, nodes_id_set):
        s = self.concretize_set(nodes_id_set)
        self.MarkNodesWhiteByID(s)
        self.MarkNodesRedByID(nodes_id_set)
        return s

    def SelectNodesByID(self, nodes):
        VizController.selectionManager.selectNodes([self.id_map[vid].getNode()
            for vid in nodes])

    def SelectNodes(self, nodes):
        VizController.selectionManager.selectNodes([v.getNode() for v in nodes])

    def ColorNodesByID(self, nodes_id, color):
        for nid in nodes_id:
            self.id_map[nid].color = color

    def ColorAllNodes(self, color):
        for n in self.gynodes:
            n.color = color;

    def MarkNodesWhiteByID(self, nodes_id):
        self.ColorNodesByID(nodes_id, jcolor.white)

    def MarkNodesRedByID(self, nodes_id):
        self.ColorNodesByID(nodes_id, jcolor.red)

    """
    Visualize a RecordableInst on the graph
    @type recinsts: List(RecordableInst)
    @param recinsts: a recording configuration
    """
    def VisualizeRecordableInst(self, recinsts):
        colorednodes = set()
        for recinst in recinsts:
            # sanity check: should not color the same node twice
            overlap = recinst.concretized_nodes & colorednodes
            if len(overlap) != 0:
                print("The following nodes will be colored twice:")
                for nid in overlap:
                    print("id: %s, label %s\n", nid, self.id_map[nid].label)
                raise RuntimeError("Color the same node twice")
            # end of sanity check
            # color 3 types of nodes in different colors:
            # 1) the nodes will be directly recorded
            # 2) the nodes will be hidden (hidden is defined in func
            # analyze_recordable)
            # 3) other nodes
            non_hidden_nodes = recinst.concretized_nodes -\
            set(recinst.rec_nodes) - set(recinst.hidden_nodes)
            self.MarkNodesRedByID(recinst.rec_nodes)
            self.MarkNodesWhiteByID(non_hidden_nodes)
            self.ColorNodesByID(recinst.hidden_nodes, jcolor.green)
            colorednodes |= recinst.concretized_nodes

    """
    Filter list of RecordableInst which can concretize the given node.
    This is helpful when you encounter a symbolic external function call.
    """
    def FilterMustRecordQuery(self, recinstsL, nid):
        filtered = []
        for recinsts in recinstsL:
            for recinst in recinsts:
                if nid in recinst.concretized_nodes:
                    filtered.append(recinsts)
                break
        return filtered


class HaseUtils(object):
    def __init__(self, globals_ref):
        self.globals = globals_ref
        self.g = self.globals['g']

        node_attrs = self.g.getNodeAttributes()
        self.attr_idepi = node_attrs['idepi']
        self.run_layout = RunForceAtlas2_nooverlap

        # important data structures
        self.maxIDep = max([v.idepi for v in self.g.nodes])
        self.idep_subg = []
        # a metric to sort root nodes from the same layer:
        # for each node n: metric(n) = sum(src.idepi if src->n has an index edge)
        # map from node.id -> score
        self.idep_index_score = {}
        self.all_root_nodes = set() # root nodes of every indirect depth layer

        # initialize important data structures
        for i in range(self.maxIDep+1):
            self.idep_subg.append(self.g.filter(self.attr_idepi==i))
        # all first level nodes is considered root
        for v in self.g.nodes:
            if v.idepi == 0:
                self.all_root_nodes.add(v.id)
                self.idep_index_score[v.id] = 0
        # nodes pointed by cross-layer edges but not by intra-layer edges are roots
        # nodes pointed by cross-layer edges will be enlarged
        for e in self.g.edges:
            if e.weight==1.5:
                if e.target.idepi == e.source.idepi + 1:
                    self.all_root_nodes.add(e.target.id)
                self.idep_index_score[e.target.id] = self.idep_index_score.get(e.target.id, 0) + e.source.idepi
        for e in self.g.edges:
            if (e.target.idepi == e.source.idepi) and \
                    (e.target.id in self.all_root_nodes):
                        self.all_root_nodes.remove(e.target.id)
                        del self.idep_index_score[e.target.id]
        self.resize_root_nodes()
        self.idep_roots = []

        """
        0 level, root nodes have special definition on this level:
          1. root nodes are nodes without indegree
          2. root nodes are sorted based on their outdegree
        """
        self.idep_roots.append(
                sorted(
                    [v for v in self.idep_subg[0].nodes if v.indegree == 0 and \
                        v.id in self.all_root_nodes],
                    key=lambda v: v.outdegree
                )
        )
        """
        non-zero level
          1. root nodes at level n are nodes pointed from level n-1
          2. root nodes are sorted based on their index_score (defined above)
        """
        for subg in self.idep_subg[1:]:
            self.idep_roots.append(
                    sorted(
                        [v for v in subg.nodes if v.id in self.all_root_nodes],
                        key=lambda v: str(self.idep_index_score.get(v.id, 0)) + v.label
                    )
            )

    """
    Focus on the n-th level of the dependency graph
    this function will:
      1. layout the root nodes of that level
      2. settle the root nodes
      3. make only current level visible
    """
    def focus_idep(self, idep):
        self.layout_idep_root(idep)
        self.lock_idep(idep)
        self.visible_idep(idep)

    def resize_root_nodes(self):
        for n in self.g.nodes:
            if n.id in self.all_root_nodes:
                n.size = 17
            else:
                n.size = 10
    """
    Layout the root nodes of a given level
    Will evenly spread roots (sorted) along Y-axis on the left most border
    (min x-axis) of that level
    """
    def layout_idep_root(self, idep, cols=1, xstep=50):
        subg = self.idep_subg[idep]
        roots = self.idep_roots[idep]
        layout_ymin = min([v.y for v in subg.nodes])
        layout_ymax = max([v.y for v in subg.nodes])
        layout_xmin = min([v.x for v in subg.nodes])
        ystep = (layout_ymax - layout_ymin) / (len(roots)//cols+1)
        for (i,r) in enumerate(roots):
            row = i//cols;
            col = i%cols;
            r.x = layout_xmin + col * xstep
            r.y = layout_ymin + (row+1) * ystep

    """
    If the spread roots are too close to each other, you may want to scale them
    along Y-axis.
    """
    def yexpand_idep_root(self, idep, ratio):
        for v in self.idep_roots[idep]:
            v.y *= ratio

    """
    Mark nodes from given level fixed (position will not change when running
    layout)
    """
    def lock_idep(self, idep):
        roots = self.idep_roots[idep]
        for r in roots:
            r.fixed = True

    """
    Mark nodes from given level non-fixed again.
    """
    def free_idep(self, idep):
        for r in self.idep_roots[idep]:
            r.fixed = False

    """
    Make nodes from given level the only nodes visible on workspace
    """
    def visible_idep(self, idep):
        self.setVisible(self.idep_subg[idep])

    """
    Move nodes with indirect depth >= `start_idep` to the right by distance `dist`
    """
    def move_right(self, start_idep, dist):
        subg = self.g.filter(self.attr_idepi >= start_idep)
        for v in subg.nodes:
            v.x += dist
    def move_right_auto(self, start_idep):
        if start_idep > 0:
            subg = self.g.filter(self.attr_idepi >= start_idep)
            cur_minx = min([v.x for v in subg.nodes])
            prev_maxx = max([v.x for v in self.idep_subg[start_idep-1].nodes])
            self.move_right(start_idep, prev_maxx + 100 - cur_minx)

    def auto_layout_all(self, iters=500, start=0):
        for i in range(start, self.maxIDep+1):
            self.focus_idep(i)
            RunForceAtlas2_nooverlap(iters)
            self.move_right_auto(i)
        self.setAllVisible()

    def auto_relayout_all(self, iters=500, start=0):
        for i in range(start, self.maxIDep+1):
            self.visible_idep(i)
            RunForceAtlas2_nooverlap(iters)
            self.move_right_auto(i)
        self.setAllVisible()

    def setVisible(self, subg):
        self.globals['visible'] = subg

    def setAllVisible(self):
        self.setVisible(self.g)

def setVisible(gs, subgraph):
    gs['visible'] = subgraph

if __name__ == "__main__":
    if (len(sys.argv) < 2):
        print("Usage: %s graph.json" % sys.argv[0])
        sys.exit()
    else:
        graph = json.load(open(sys.argv[1]))
        h = PyGraph.buildFromPyDict(graph)
        print("%d nodes, %d edges, max idep %d" % (len(h.gynodes),
            len(h.gyedges), h.max_idep()))
        r = h.analyze_recordable()
        query_nodes = list(filter(lambda n: n.category == "Q", h.gynodes))
        if len(query_nodes) > 0:
            for n in query_nodes:
                filtered = h.FilterMustRecordQuery(r, n.id)
                print("Query Expression with kinst \"%s\" can be "
                      "covered by:" % (n.kinst))
                h.printCandidateRecInstsInfo(filtered)
        else:
            sr = h.sortRecInstsbyCoverageScore(r)
            srf = h.sortRecInstsbyCoverageScoreFreq(r)
            print("Coverage Score Highest 5:")
            h.printCandidateRecInstsInfo(sr[-5:])
            print("Coverage Freq Score Highest 5:")
            h.printCandidateRecInstsInfo(srf[-5:])
