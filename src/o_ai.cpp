#include "o_ai.hpp"
#include "o.hpp"

#include <array>

#include <boost/container/small_vector.hpp>

#include "flat/flat_map.hpp"
#include "flat/small_set.hpp"

#include "alloca.hpp"
#include "bitset.hpp"
#include "fixed.hpp"
#include "ir.hpp"
#include "o_phi.hpp"
#include "sizeof_bits.hpp"
#include "worklist.hpp"

namespace bc = ::boost::container;

#include <iostream> // TODO
#include <bitset> // TODO

namespace {

// These are used as indices into 'executable' and 'output_executable' arrays.
enum executable_index_t
{
    EXEC_PROPAGATE = 0,
    EXEC_JUMP_THREAD = 1,
};

struct cfg_ai_d
{
    // This bitset tracks if the abstract interpreter has executed along
    // a given output edge.
    std::array<std::uint64_t, 2> output_executable = {};
    static constexpr unsigned max_output_size = 64; // Number of bits in bitset 

    // This tracks if the owning CFG node has been executed.
    std::array<bool, 2> executable = {};
    
    // Used in branch threading to track the path.
    unsigned input_taken = 0;

    // Used to rebuild the SSA after inserting trace nodes.
    using rebuild_map_t = fc::vector_map<ssa_ht, ssa_ht>;
    rebuild_map_t rebuild_map;

    // Tracks if the node can be skipped over by the jump threading pass.
    // (A node can be skipped over if it contains no code that would need
    //  to be duplicated along the threaded jump.)
    // Also used to mark CFG nodes created by the trace pass - these will
    // get removed after the optimization runs!
    bool skippable = false;
};

struct ssa_ai_d
{
    // The imprecise set of all values this node can have during runtime.
    std::array<constraints_def_t, 2> constraints_array;
    unsigned constraints_i = 0;

    // If this node is a key to 'ai_cfg_data_t::rebuild_map', this pointer
    // holds the mapped value.
    // i.e. it holds the original value.
    ssa_ht rebuild_mapping = {};

    // How many times this node has been visited by the abstract interpreter.
    // This is used to determine when to widen.
    unsigned visited_count = 0;

    // If any of the value's inputs were modified by the jump threading pass.
    bool touched = false;

    constraints_def_t& constraints() 
        { return constraints_array[constraints_i]; } 
    void set_active_constraints(executable_index_t e) { constraints_i = e; }
};

cfg_ai_d& ai_data(cfg_ht h) { return h.data<cfg_ai_d>(); }
ssa_ai_d& ai_data(ssa_ht h) { return h.data<ssa_ai_d>(); }

} // End anonymous namespace

namespace // Anonymous namespace
{

std::size_t constraints_size(ssa_node_t const& node)
{
    switch(node.op())
    {
    case SSA_add:
    case SSA_sub:
        return 2; // Second constraint is for the carry.
    case SSA_trace:
        return constraints_size(*node.input(0));
    default:
        if(is_array_like(node.type()))
            return node.type().size();
        return is_numeric(node.type()) ? 1 : 0;
    }
}

bool has_constraints(ssa_ht node)
{
    assert(node);
    return ai_data(node).constraints().vec.size();
}

bool has_constraints(ssa_value_t v)
{
    if(v.is_handle())
        return has_constraints(v.handle());
    return v.is_num();
}

void copy_constraints(ssa_value_t value, constraints_def_t& vec)
{
    if(value.is_handle())
        vec = ai_data(value.handle()).constraints();
    else if(value.is_num())
        vec = { numeric_bitmask(TYPE_LARGEST_FIXED), 
                { constraints_t::const_(value.fixed().value) }};
    else
        vec = {};
}

constraints_t first_constraint(ssa_value_t value)
{
    if(value.is_handle())
    {
        assert(has_constraints(value.handle()));
        return ai_data(value.handle()).constraints()[0];
    }
    else if(value.is_num())
        return constraints_t::const_(value.fixed().value);
    assert(false);
    return constraints_t::top();
}

// AI = abstract interpretation, NOT artificial intelligence.
struct ai_t
{
public:
    explicit ai_t(ir_t&);

private:
    // Threshold points where widening occurs.
    // Keep these in ascending order!!
    static constexpr unsigned WIDEN_OP_BOUNDS = 16;
    static constexpr unsigned WIDEN_OP        = 24;

    void mark_skippable();
    void remove_skippable();

    ssa_ht local_lookup(cfg_ht cfg_h, ssa_ht ssa_h);
    void insert_trace(cfg_ht cfg_trace_h, ssa_ht original_h, 
                      ssa_value_t parent_trace, unsigned arg_i);
    void insert_traces();

    void queue_edge(cfg_ht h, unsigned out_i);
    void queue_node(executable_index_t exec_i, ssa_ht h);

    void compute_trace_constraints(executable_index_t exec_i, ssa_ht trace_h);
    void compute_constraints(executable_index_t exec_i, ssa_ht ssa_h);
    void visit(ssa_ht ssa_h);
    void range_propagate();
    void prune_unreachable_code();
    void fold_consts();

    // Jump threading
    void jump_thread_visit(ssa_ht ssa_h);
    void run_jump_thread(cfg_ht start_h, unsigned start_branch_i);
    void thread_jumps();

    ir_t& ir;

    std::vector<ssa_ht> needs_rebuild;
    std::vector<cfg_ht> threaded_jumps;

public:
    bool updated = false;
};

ai_t::ai_t(ir_t& ir_) : ir(ir_)
{
    // Currently, the AI implementation has a limit on the number of
    // output edges a node can have. This could be worked around, but 
    // it's rare in practice and simpler to code this way.
    for(cfg_node_t& node : ir)
        if(node.output_size() > cfg_ai_d::max_output_size)
            return; // TODO: handle this better

    ir.assert_valid();

    std::puts("TRACE");
    insert_traces();
    ir.assert_valid();

    std::puts("PROPAGATE");
    range_propagate();
    ir.assert_valid();

    std::puts("PRUNE");
    prune_unreachable_code();
    ir.assert_valid();

    std::puts("MARK SKIP");
    mark_skippable();
    ir.assert_valid();

    std::puts("THREAD");
    thread_jumps();
    ir.assert_valid();
    
    std::puts("FOLD");
    fold_consts();
    ir.assert_valid();

    std::puts("REMOVE SKIP");
    remove_skippable();
    ir.assert_valid();
}

////////////////////////////////////////
// HELPERS                            //
////////////////////////////////////////

void ai_t::queue_edge(cfg_ht h, unsigned out_i)
{
    auto& d = ai_data(h);

    if(d.output_executable[EXEC_PROPAGATE] & (1ull << out_i))
        return;

    d.output_executable[EXEC_PROPAGATE] |= (1ull << out_i);
    cfg_worklist.push(h->output(out_i));
}

void ai_t::queue_node(executable_index_t exec_i, ssa_ht h)
{
    if(ai_data(h->cfg_node()).executable[exec_i])
        ssa_worklist.push(h);
}

////////////////////////////////////////
// SKIPPABLE                          //
////////////////////////////////////////

void ai_t::mark_skippable()
{
    // A skippable CFG is one where every SSA node is either:
    // - A node inserted during SSA reconstruction
    // - A node that is used in its own CFG node but not anywhere else
    // These CFG nodes are what can be skipped over by jump threading.
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it; ++cfg_it)
    {
        assert(cfg_it->test_flags(FLAG_IN_WORKLIST) == false);

        for(ssa_ht ssa_it = cfg_it->ssa_begin(); ssa_it; ++ssa_it)
        {
            auto& d = ai_data(ssa_it);

            if(d.rebuild_mapping)
                continue;
            assert(ssa_it->op() != SSA_trace);

            unsigned const output_size = ssa_it->output_size();
            for(unsigned i = 0; i < output_size; ++i)
            {
                ssa_node_t& output = *ssa_it->output(i);
                if(output.cfg_node() != cfg_it && output.op() != SSA_trace)
                    goto not_skippable;
            }
        }

        // Mark it as skippable!
        ai_data(cfg_it).skippable = true;
    not_skippable:;
    }
}

// Prunes nodes found by 'mark_skippable' -- but only when they are
// nodes with 1 input and 1 output.
void ai_t::remove_skippable()
{
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it;)
    {
        if((ai_data(cfg_it).skippable)
           && cfg_it->input_size() == 1 && cfg_it->output_size() == 1)
        {
            // Before pruning the CFG node, all SSA nodes it contains must be
            // pruned too.
            while(ssa_ht ssa_it = cfg_it->ssa_begin())
            {
                // Nodes inserted during the SSA rebuild can simply be
                // replaced with their original value.
                if(ssa_ht mapping = ai_data(ssa_it).rebuild_mapping)
                    ssa_it->replace_with(mapping);

                ssa_it->prune();
            }

            // Prune the skippable node.
            cfg_it = ir.merge_edge(cfg_it);
        }
        else
            ++cfg_it;
    }
}

////////////////////////////////////////
// TRACES                             //
////////////////////////////////////////

// For inserting traces, we use the same SSA-generation algorithm
// we used to generate the SSA from the AST. The difference is we
// lookup existing SSA nodes instead of local variables.
// Relevant paper:
//   Simple and Efficient Construction of Static Single Assignment Form
ssa_ht ai_t::local_lookup(cfg_ht cfg_node, ssa_ht ssa_node)
{
    if(ssa_node->cfg_node() == cfg_node)
        return ssa_node;

    auto& cd = ai_data(cfg_node);
    auto lookup = cd.rebuild_map.find(ssa_node);

    if(lookup != cd.rebuild_map.end())
        return lookup->second;
    else
    {
        // If 'cfg_node' doesn't contain a definition for 'ssa_node',
        // recursively look up its definition in predecessor nodes.
        // If there are multiple predecessors, a phi node will be created.
        switch(cfg_node->input_size())
        {
        case 0:
            throw std::runtime_error("Local lookup failed.");
        case 1:
            return local_lookup(cfg_node->input(0), ssa_node);
        default:
            ssa_ht phi = cfg_node->emplace_ssa(SSA_phi, ssa_node->type());
            // TODO:
            ssa_data_pool::resize<ssa_ai_d>(ssa_pool::array_size());

            ai_data(phi).rebuild_mapping = ssa_node;
            cd.rebuild_map.emplace(ssa_node, phi);

            // Fill using local lookups:
            unsigned const input_size = cfg_node->input_size();
            phi->alloc_input(input_size);
            for(unsigned i = 0; i < input_size; ++i)
                phi->build_set_input(
                    i, local_lookup(cfg_node->input(i), ssa_node));

            return phi;
        }
    }
}

void ai_t::insert_trace(cfg_ht cfg_trace, ssa_ht original, 
                        ssa_value_t parent_trace, unsigned arg_i)
{
    auto& cfg_trace_d = ai_data(cfg_trace);
    auto& rebuild_map = cfg_trace_d.rebuild_map;

    // A single node can appear multiple times in the condition expression.
    // Check to see if that's the case by checking if a trace already exists
    // for this node.
    auto it = rebuild_map.find(original);
    if(it != rebuild_map.end())
    {
        // The trace already exists.
        ssa_ht h = it->second;
        assert(parent_trace.is_handle());
        if(h != parent_trace.handle())
        {
            h->link_append_input(parent_trace);
            h->link_append_input(arg_i);
        }
        return;
    }

    ssa_ht trace = cfg_trace->emplace_ssa(SSA_trace, original->type());
    //TODO:
    ssa_data_pool::resize<ssa_ai_d>(ssa_pool::array_size());
    // All references to SSA nodes have been invalidated by the new node!!

    rebuild_map.insert({ original, trace });

    auto& d = ai_data(trace);
    assert(original);
    d.rebuild_mapping = original;

    if(parent_trace.is_handle())
    {
        trace->alloc_input(3);

        // The first argument is the original expression it represents.
        trace->build_set_input(0, original);

        // The remaining arguments come in pairs.
        // - First comes the parent trace.
        // - Second comes the argument index into the parent trace.
        trace->build_set_input(1, parent_trace);
        trace->build_set_input(2, arg_i);
    }
    else
    {
        // If 'parent_trace' is a constant, that means this is the first
        // trace in the trace-graph. We represent this node slightly
        // differently, using only the first two arguments.
        trace->alloc_input(2);
        trace->build_set_input(0, original);
        trace->build_set_input(1, parent_trace);
    }

    if(ssa_flags(original->op()) & SSAF_TRACE_INPUTS)
    {
        // Recursively trace 'original', turning its inputs into traces too.
        unsigned const input_size = original->input_size();
        for(unsigned i = 0; i < input_size; ++i)
        {
            ssa_value_t input = original->input(i);
            if(input.holds_ref())
                insert_trace(cfg_trace, input.handle(), trace, i);
        }
    }

    // All outputs of the original node will need rebuilding.
    needs_rebuild.push_back(original);
}

void ai_t::insert_traces()
{
    for(cfg_ht cfg_branch = ir.cfg_begin(); cfg_branch; ++cfg_branch)
    {
        // A branch is any cfg node with more than 1 successor.
        unsigned const output_size = cfg_branch->output_size();
        if(output_size != 2) // TODO: handle switch
            continue;

        assert(cfg_branch->last_daisy());
        ssa_node_t& ssa_branch = *cfg_branch->last_daisy();
        assert(ssa_branch.op() == SSA_if);

        // If the condition is const, there's no point
        // in making a trace partition out of it.
        ssa_value_t condition = get_condition(ssa_branch);
        if(!condition.is_handle())
            continue;

        // Create new CFG nodes along each branch and insert SSA_traces
        // into them.
        for(unsigned i = 0; i < output_size; ++i)
        {
            cfg_ht cfg_trace = ir.split_edge(cfg_branch->output_edge(i));
            cfg_data_pool::resize<cfg_ai_d>(cfg_pool::array_size());
            insert_trace(cfg_trace, condition.handle(), i, 0);
        }
    }

    ir.assert_valid();

    // For all the nodes that spawned a trace,
    // modify the inputs of their outputs to use the trace.
    for(ssa_ht h : needs_rebuild)
    {
        auto& d = ai_data(h);
        ssa_ht const look_for = d.rebuild_mapping ? d.rebuild_mapping : h;

        for(unsigned i = 0; i < h->output_size();)
        {
            ssa_bck_edge_t edge = h->output_edge(i);
            assert(edge.handle->op() != SSA_trace || edge.index == 0);

            ssa_ht lookup = local_lookup(edge.handle->input_cfg(edge.index), 
                                         look_for);

            if(!edge.handle->link_change_input(edge.index, lookup))
               ++i;
        }
    }

    needs_rebuild.clear();
}

// Doesn't normalize (to handle widening more efficiently).
// You must normalize at call site.
void ai_t::compute_trace_constraints(executable_index_t exec_i, ssa_ht trace)
{
    auto& trace_d = ai_data(trace);

    // If there's only two arguments that means we have a 'root' trace.
    // i.e. the trace's original node is the input to the branch node.
    // The constraints of this is always constant.
    if(trace->input_size() == 2)
    {
        assert(trace->input(1).is_const());
        trace_d.constraints() =
        { 
            numeric_bitmask(TYPE_LARGEST_FIXED),
            { constraints_t::const_(trace->input(1).fixed().value) } 
        };
        return;
    }

    assert(trace->input_size() > 2);
    assert(trace->input_size() % 2 == 1);

    // Do a quick check to make sure all our inputs are non-top.
    unsigned const input_size = trace->input_size();
    for(unsigned i = 1; i < input_size; i += 2)
    {
        ssa_ht parent_trace = trace->input(i).handle();
        assert(parent_trace->op() == SSA_trace);
        if(any_top(ai_data(parent_trace).constraints().vec))
            return;
    }

    bc::small_vector<constraints_def_t, 16> c;

    // For each parent, perform a narrowing operation.
    constraints_vec_t narrowed(
        trace_d.constraints().vec.size(),
        constraints_t::bottom(trace_d.constraints().mask));
    for(unsigned i = 1; i < input_size; i += 2)
    {
        ssa_ht parent_trace = trace->input(i).handle();
        ssa_ht parent_original = parent_trace->input(0).handle();

        unsigned const arg_i = trace->input(i+1).whole();
        unsigned const num_args = parent_original->input_size();

        // The narrow function expects a mutable array of constraints
        // that it modifies. Create that array here.
        c.resize(num_args);
        for(unsigned j = 0; j < num_args; ++j)
            copy_constraints(parent_original->input(j), c[j]);

        auto& parent_trace_d = ai_data(parent_trace);
        assert(parent_trace_d.rebuild_mapping);
        ssa_op_t const op = parent_original->op();
        assert(narrow_fn(op));
        // Call the narrowing op:
        narrow_fn(op)(c.data(), num_args, parent_trace_d.constraints());

        assert(c[arg_i].vec.size() == narrowed.size());
        for(unsigned j = 0; j < narrowed.size(); ++j)
            narrowed[j] = intersect(narrowed[j], c[arg_i][j]);
    }

    trace_d.set_active_constraints(exec_i);
    for(unsigned i = 0; i < narrowed.size(); ++i)
        trace_d.constraints()[i] = union_(trace_d.constraints()[i],
                                          narrowed[i]);
}

////////////////////////////////////////
// RANGE PROPAGATION                  //
////////////////////////////////////////

// Doesn't normalize (to handle widening more efficiently).
// You must normalize at call site.
void ai_t::compute_constraints(executable_index_t exec_i, ssa_ht ssa_node)
{
    auto& d = ai_data(ssa_node);

    if(ssa_node->op() == SSA_trace)
        compute_trace_constraints(exec_i, ssa_node);
    else
    {
        // Build an array holding all the argument's constraints.
        unsigned const input_size = ssa_node->input_size();
        bc::small_vector<constraints_def_t, 16> c;
        c.resize(input_size);
        if(ssa_node->op() == SSA_phi)
        {
            // For phi nodes, if an input CFG edge hasn't been marked
            // executable, treat the argument's constraint as TOP.
            cfg_ht cfg_node = ssa_node->cfg_node();

            assert(input_size == cfg_node->input_size());
            for(unsigned i = 0; i < input_size; ++i)
            {
                auto edge = cfg_node->input_edge(i);
                auto& edge_d = ai_data(edge.handle);

                if(edge_d.output_executable[exec_i] & (1ull << edge.index))
                    copy_constraints(ssa_node->input(i), c[i]);
                else
                    c[i].vec.assign(d.constraints().vec.size(), 
                                    constraints_t::top());
            }
        }
        else for(unsigned i = 0; i < input_size; ++i)
            copy_constraints(ssa_node->input(i), c[i]);

        // Call the ai op:
        assert(abstract_fn(ssa_node->op()));
        d.set_active_constraints(exec_i);
        abstract_fn(ssa_node->op())(c.data(), input_size, d.constraints());
    }
}

// Performs range propagatation on a single SSA node.
void ai_t::visit(ssa_ht ssa_node)
{
    std::cout << "visit " << to_string(ssa_node->op()) << '\n';

    if(ssa_node->op() == SSA_if)
    {
        ssa_value_t condition = get_condition(*ssa_node);

        assert(has_constraints(condition));
        assert(ssa_node->cfg_node()->output_size() == 2);

        constraints_t c = first_constraint(condition);
        std::cout << "COND = " << c << '\n';

        if(c.is_top())
            return;

        if(!c.is_const())
        {
            queue_edge(ssa_node->cfg_node(), 0);
            queue_edge(ssa_node->cfg_node(), 1);
        }
        else if(c.get_const())
            queue_edge(ssa_node->cfg_node(), 1);
        else
            queue_edge(ssa_node->cfg_node(), 0);

        return;
    }
    else if(!has_constraints(ssa_node))
        return;

    auto& d = ai_data(ssa_node);

    thread_local constraints_vec_t old_constraints;
    old_constraints = d.constraints().vec;
    assert(all_normalized(old_constraints));

    if(d.visited_count >= WIDEN_OP)
    {
        d.constraints().vec.assign(
            d.constraints().vec.size(), 
            constraints_t::bottom(d.constraints().mask));
    }
    else
    {
        compute_constraints(EXEC_PROPAGATE, ssa_node);
        if(d.visited_count > WIDEN_OP_BOUNDS)
            for(constraints_t& c : d.constraints().vec)
                c.bounds = bounds_t::bottom(d.constraints().mask);
        for(constraints_t& c : d.constraints().vec)
            c.normalize();
    }

    // TODO:
    std::cout << "C = " << d.constraints()[0] << '\n';
    std::cout << "O = " << old_constraints[0] << '\n';

    assert(all_normalized(d.constraints().vec));
    if(!bit_eq(d.constraints().vec, old_constraints))
    {
        assert(all_subset(old_constraints, d.constraints().vec));

        // Update the visited count. Traces increment twice as fast, which
        // was chosen to improve widening behavior.
        d.visited_count += ssa_node->op() == SSA_trace ? 2 : 1;

        // Queue all outputs
        unsigned const output_size = ssa_node->output_size();
        for(unsigned i = 0; i < output_size; ++i)
            queue_node(EXEC_PROPAGATE, ssa_node->output(i));
    }
}

void ai_t::range_propagate()
{
    assert(ssa_worklist.empty());
    assert(cfg_worklist.empty());

    // Reset the flags.
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it; ++cfg_it)
    {
        auto& cd = ai_data(cfg_it);
        cd.skippable = false;
        assert(cfg_it->test_flags(FLAG_IN_WORKLIST) == false);
        assert(cd.output_executable[EXEC_PROPAGATE] == 0ull);

        for(ssa_ht ssa_it = cfg_it->ssa_begin(); ssa_it; ++ssa_it)
        {
            auto& sd = ai_data(ssa_it);
            sd.touched = false;

            assert(ssa_it->test_flags(FLAG_IN_WORKLIST) == false);
            assert(sd.visited_count == 0);
            assert(sd.constraints_i == EXEC_PROPAGATE);

            constraints_def_t& constraints = sd.constraints();
            type_t const type = ssa_it->type();

            constraints.vec.clear();
            constraints.vec.resize(constraints_size(*ssa_it),
                                   constraints_t::top());

            if(constraints.vec.empty())
                continue;

            if(type.name() == TYPE_ARRAY)
                constraints.mask = numeric_bitmask(type[0]);
            else if(type.name() == TYPE_BUFFER)
                constraints.mask = numeric_bitmask(TYPE_BYTE);
            else
                sd.constraints().mask = numeric_bitmask(ssa_it->type());
        }
    }

    cfg_worklist.push(ir.root);

    ir.assert_valid();
    while(!ssa_worklist.empty() || !cfg_worklist.empty())
    {
        while(!ssa_worklist.empty())
            visit(ssa_worklist.pop());

        while(!cfg_worklist.empty())
        {
            cfg_ht cfg_node = cfg_worklist.pop();
            std::printf("CFG VISIT %i\n", cfg_node.index);
            auto& d = ai_data(cfg_node);

            if(!d.executable[EXEC_PROPAGATE])
            {
                d.executable[EXEC_PROPAGATE] = true;

                // Visit all expressions in this node.
                for(ssa_ht ssa_it = cfg_node->ssa_begin(); ssa_it; ++ssa_it)
                    queue_node(EXEC_PROPAGATE, ssa_it);
            }
            else for(auto phi_it = cfg_node->phi_begin(); phi_it; ++phi_it)
            {
                // Visit all phis in this node
                assert(phi_it->op() == SSA_phi);
                queue_node(EXEC_PROPAGATE, phi_it);
            }

            // Queue successor cfg nodes.
            if(cfg_node->output_size() == 1)
                queue_edge(cfg_node, 0);
        }
    }
    ir.assert_valid();
}

////////////////////////////////////////
// PRUNING                            //
////////////////////////////////////////

void ai_t::prune_unreachable_code()
{
    // Replace branches with constant conditionals with non-branching jumps.
    for(cfg_node_t& cfg_node : ir)
    {
        if(cfg_node.output_size() != 2) // TODO: handle switch
            continue;

        ssa_ht branch = cfg_node.last_daisy();
        assert(branch && branch->op() == SSA_if);

        constraints_t c = first_constraint(get_condition(*branch));
        if(!c.is_const())
            continue;

        // First calculate the branch index to remove.
        bool const prune_i = !(c.get_const() >> fixed_t::shift);

        // Then remove the conditional.
        branch->prune();
        assert(cfg_node.last_daisy() != branch);

        // Finally remove the branch from the CFG node.
        cfg_node.link_remove_output(prune_i);
        assert(cfg_node.output_size() == 1);

        updated = true;
    }

    ir.assert_valid();

    // Remove all CFG nodes that weren't executed by the abstract interpreter.
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it;)
    {
        if(ai_data(cfg_it).executable[EXEC_PROPAGATE])
            ++cfg_it;
        else
        {
            cfg_it = ir.prune_cfg(cfg_it);
            updated = true;
        }
    }

    ir.assert_valid();
}

void ai_t::fold_consts()
{
    // Replace nodes determined to be constant with a constant ssa_value_t.
    for(cfg_node_t& cfg_node : ir)
    for(ssa_ht ssa_it = cfg_node.ssa_begin(); ssa_it; ++ssa_it)
    {
        if(ssa_it->op() == SSA_trace)
            std::puts(" xx TRACe xx ");

        if(ssa_it->output_size() == 0 || !has_constraints(ssa_it))
            continue;

        ssa_op_t const op = ssa_it->op();
        auto& d = ai_data(ssa_it);

        if(is_numeric(ssa_it->type()) && d.constraints()[0].is_const())
        {
            fixed_t constant = { d.constraints()[0].get_const() };
            std::cout << " FOLDING " << ssa_it->op() << ' ' << (constant.value >> fixed_t::shift) << ' ' << ssa_it->output_size() << '\n';
            if(ssa_it->replace_with(INPUT_VALUE, constant))
                updated = true;
            std::cout << " CONT " << ssa_it->op() << ' ' << ssa_it->output_size() << '\n';
        }
        else if(op == SSA_eq || op == SSA_not_eq)
        {
            // Simplify comparisons, removing unnecessary operations:
            for(unsigned i = 0; i < ssa_it->input_size();)
            {
                assert(ssa_it->input_size() % 2 == 0);

                ssa_value_t const lhs = ssa_it->input(i);
                ssa_value_t const rhs = ssa_it->input(i+1);

                constraints_t const lhs_c = first_constraint(lhs);
                constraints_t const rhs_c = first_constraint(rhs);

                if(lhs_c.is_const() && rhs_c.is_const()
                   && (lhs_c.get_const() == rhs_c.get_const())
                      == (op == SSA_eq))
                {
                    ssa_it->link_remove_input(i+1);
                    ssa_it->link_remove_input(i);
                    updated = true;
                    continue;
                }

                i+= 2;
            }
        }
        else if(op == SSA_lt || op == SSA_lte)
        {
            // Simplify comparisons, removing unnecessary operations:
            while(unsigned const size = ssa_it->input_size())
            {
                ssa_value_t const lhs = ssa_it->input(size - 2);
                ssa_value_t const rhs = ssa_it->input(size - 1);

                constraints_t const lhs_c = first_constraint(lhs);
                constraints_t const rhs_c = first_constraint(rhs);

                if(lhs_c.is_const() && rhs_c.is_const() 
                   && lhs_c.get_const() == rhs_c.get_const())
                {
                    ssa_it->link_shrink_inputs(size - 2);
                    updated = true;
                }
                else
                    break;
            }
        }
    }
    ir.assert_valid();
}

////////////////////////////////////////
// JUMP THREADING                     //
////////////////////////////////////////

void ai_t::jump_thread_visit(ssa_ht ssa_node)
{
    if(!has_constraints(ssa_node))
        return;

    auto& d = ai_data(ssa_node);

    // TODO
    constraints_def_t const old_constraints = d.constraints();
    assert(all_normalized(old_constraints.vec));

    compute_constraints(EXEC_JUMP_THREAD, ssa_node);
    for(constraints_t& c : d.constraints().vec)
        c.normalize();

    if(!bit_eq(d.constraints().vec, old_constraints.vec))
    {
        // Queue all outputs
        unsigned const output_size = ssa_node->output_size();
        for(unsigned i = 0; i < output_size; ++i)
        {
            ssa_ht output = ssa_node->output(i);
            ai_data(output).touched = true;
            queue_node(EXEC_JUMP_THREAD, output);
        }
    }
}

void ai_t::run_jump_thread(cfg_ht start, unsigned start_branch_i)
{
    // Reset the state.
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it; ++cfg_it)
    {
        auto& cd = ai_data(cfg_it);

        cd.executable[EXEC_JUMP_THREAD] = false;
        cd.output_executable[EXEC_JUMP_THREAD] = 0ull;

        for(ssa_ht ssa_it = cfg_it->ssa_begin(); ssa_it; ++ssa_it)
        {
            auto& sd = ai_data(ssa_it);

            sd.set_active_constraints(EXEC_PROPAGATE);
            sd.touched = false;
            assert(ssa_it->test_flags(FLAG_IN_WORKLIST) == false);
        }
    }

    ai_data(start).executable[EXEC_JUMP_THREAD] = true;

    // Step through CFG nodes, taking forced paths until reaching a branch
    // that is not forced.

    cfg_ht h = start;
    unsigned branch_i = start_branch_i;
    unsigned branches_skipped = 0;
    while(true)
    {
        cfg_node_t& prior_node = *h;
        auto& prior_d = ai_data(h);

        // Take the branch.
        prior_d.output_executable[EXEC_JUMP_THREAD] |= 1ull << branch_i;
        unsigned const input_i = prior_node.output_edge(branch_i).index;
        h = prior_node.output(branch_i);

        cfg_node_t& cfg_node = *h;
        auto& cfg_data = ai_data(h);

        cfg_data.input_taken = input_i;

        // If we've ended up in a loop, abort!
        if(cfg_data.executable[EXEC_JUMP_THREAD])
            break;

        cfg_data.executable[EXEC_JUMP_THREAD] = true;

        // If we've reached an unskippable, abort!
        if(!cfg_data.skippable)
            break;

        // If we've reached an endpoint node, abort!
        if(cfg_node.output_size() == 0)
            break;

        // Queue and then update all relevant SSA nodes in this CFG node.
        for(auto ssa_it = cfg_node.ssa_begin(); ssa_it; ++ssa_it)
            if(ssa_it->op() == SSA_phi || ai_data(ssa_it).touched)
                queue_node(EXEC_JUMP_THREAD, ssa_it);

        // Here's where we update the SSA nodes:
        while(!ssa_worklist.empty())
            jump_thread_visit(ssa_worklist.pop());

        // Check to see if the next path is forced, taking it if it is.
        assert(cfg_node.output_size() != 0); // Handled earlier.
        if(cfg_node.output_size() > 1)
        {
            assert(cfg_node.output_size() == 2); // TODO: handle switch

            ssa_ht branch = cfg_node.last_daisy();
            assert(branch && branch->op() == SSA_if);

            constraints_t const c = first_constraint(get_condition(*branch));
            if(!c.is_const())
                break;
            branch_i = c.get_const() >> fixed_t::shift;
            ++branches_skipped;
        }
        else // Non-conditional nodes are always forced
            branch_i = 0;
    }

    if(branches_skipped == 0)
        return;

    cfg_ht end = h;

    // We've found the path. Now modify the IR.

    cfg_ht trace = start->output(start_branch_i);

    assert(trace->output_size() == 1);
    trace->link_append_output(end, [&](ssa_ht phi) -> ssa_value_t
    {
        // Phi nodes in the target need a new input argument.
        // Find that here by walking the path backwards until
        // reaching a node outside of the path or in the first path node.
        ssa_ht v = phi;
        while(true)
        {
            if(v->op() != SSA_phi || v->cfg_node() == start)
                break;

            auto& d = ai_data(v->cfg_node());

            if(!d.executable[EXEC_JUMP_THREAD])
                break;

            unsigned input_i = d.input_taken;
            ssa_value_t input = v->input(input_i);

            if(input.is_const())
                return input;

            v = input.handle();
            if(ssa_ht mapping = ai_data(v).rebuild_mapping)
                v = mapping;
        }
        assert(v->cfg_node() == start
               || !(ai_data(v->cfg_node()).executable[EXEC_JUMP_THREAD]));
        return v;
    });

    threaded_jumps.push_back(trace);
}

void ai_t::thread_jumps()
{
    // Find all jump threads, creating new edges and storing the endpoints in
    // 'threaded_jumps'.
    threaded_jumps.clear();
    for(cfg_ht cfg_it = ir.cfg_begin(); cfg_it; ++cfg_it)
    {
        if(!ai_data(cfg_it).skippable)
            continue;

        if(cfg_it->output_size() <= 1)
            continue;

        // Ok! 'cfg_node' is a jump thread target.

        unsigned const input_size = cfg_it->input_size();
        for(unsigned i = 0; i < input_size; ++i)
        {
            // Traverse until finding a non-forced node.
            cfg_fwd_edge_t input = cfg_it->input_edge(i);
            while(true)
            {
                cfg_node_t& node = *input.handle;

                if(!ai_data(input.handle).skippable)
                    break;

                if(node.input_size() != 1 || node.output_size() != 1)
                    break;

                input = node.input_edge(0);
            }

            // For the time being, only handle 'if' nodes.
            // TODO: support switch
            if(input.handle->output_size() != 2)
                continue;

            std::puts("running jump thread");
            run_jump_thread(input.handle, input.index);
        }
    }

    std::cout << "THREADS: " << threaded_jumps.size() << '\n';

    if(threaded_jumps.size() == 0)
        return;
    updated = true;

    // Remove prior edges that are no longer used.
    assert(cfg_worklist.empty());
    for(cfg_ht jump : threaded_jumps)
    {
        assert(jump->output_size() == 2);
        cfg_worklist.push(jump); // For the next step.
        jump->link_remove_output(0);
    }

    // Prune unreachable nodes with no inputs here.
    // (Jump threading can create such nodes)
    while(!cfg_worklist.empty())
    {
        cfg_ht cfg_node = cfg_worklist.pop();

        if(cfg_node->input_size() == 0)
        {
            unsigned const output_size = cfg_node->output_size();
            for(unsigned i = 0; i < output_size; ++i)
            {
                assert(cfg_node->output(i) != cfg_node);
                cfg_worklist.push(cfg_node->output(i));
            }
            ir.prune_cfg(cfg_node);
        }
    }
}

} // End anonymous namespace

bool o_abstract_interpret(ir_t& ir)
{
    cfg_data_pool::scope_guard_t<cfg_ai_d> cg(cfg_pool::array_size());
    ssa_data_pool::scope_guard_t<ssa_ai_d> sg(ssa_pool::array_size());
    ai_t ai(ir);
    return ai.updated;
}
