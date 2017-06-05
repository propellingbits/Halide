#include "AsyncProducers.h"
#include "ExprUsesVar.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "IROperator.h"

namespace Halide {
namespace Internal {

using std::vector;
using std::set;
using std::pair;
using std::string;
using std::map;

/** A mutator which eagerly folds no-op stmts */
class NoOpCollapsingMutator : public IRMutator {
protected:

    using IRMutator::visit;

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    void visit(const For *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = For::make(op->name, op->min, op->extent, op->for_type, op->device_api, body);
        }
    }

    void visit(const Block *op) {
        Stmt first = mutate(op->first);
        Stmt rest = mutate(op->rest);
        if (is_no_op(first)) {
            stmt = rest;
        } else if (is_no_op(rest)) {
            stmt = first;
        } else {
            stmt = Block::make(first, rest);
        }
    }

    void visit(const Fork *op) {
        Stmt first = mutate(op->first);
        Stmt rest = mutate(op->rest);
        if (is_no_op(first)) {
            stmt = rest;
        } else if (is_no_op(rest)) {
            stmt = first;
        } else {
            stmt = Fork::make(first, rest);
        }
    }

    void visit(const Realize *op) {
        Stmt body = mutate(op->body);
        if (is_no_op(body)) {
            stmt = body;
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds,
                                 op->condition, body);
        }
    }

    void visit(const IfThenElse *op) {
        Stmt then_case = mutate(op->then_case);
        Stmt else_case = mutate(op->else_case);
        if (is_no_op(then_case) && is_no_op(else_case)) {
            stmt = then_case;
        } else {
            stmt = IfThenElse::make(op->condition, then_case, else_case);
        }
    }
};

class GenerateProducerBody : public NoOpCollapsingMutator {
    const string &func;
    vector<Expr> sema;

    using NoOpCollapsingMutator::visit;

    // Preserve produce nodes and add synchronization
    void visit(const ProducerConsumer *op) {
        if (op->name == func && op->is_producer) {
            // Add post-synchronization
            internal_assert(!sema.empty()) << "Duplicate produce node!\n";
            Stmt body = op->body;
            while (!sema.empty()) {
                Expr release = Call::make(Int(32), "halide_semaphore_release", {sema.back(), 1}, Call::Extern);
                body = Block::make(body, Evaluate::make(release));
                sema.pop_back();
            }
            stmt = ProducerConsumer::make_produce(op->name, body);
        } else {
            Stmt body = mutate(op->body);
            if (is_no_op(body) || op->is_producer) {
                stmt = body;
            } else {
                stmt = ProducerConsumer::make(op->name, op->is_producer, body);
            }
        }
    }

    // Other stmt leaves get replaced with no-ops
    void visit(const Evaluate *) {
        stmt = Evaluate::make(0);
    }

    void visit(const Provide *) {
        stmt = Evaluate::make(0);
    }

    void visit(const AssertStmt *) {
        stmt = Evaluate::make(0);
    }

    void visit(const Prefetch *) {
        stmt = Evaluate::make(0);
    }

    void visit(const Acquire *op) {
        Stmt body = mutate(op->body);
        const Variable *var = op->semaphore.as<Variable>();
        internal_assert(var);
        if (is_no_op(body)) {
            stmt = body;
        } else if (starts_with(var->name, func + ".folding_semaphore.")) {
            // This is a storage-folding semaphore for the func we're producing. Keep it.
            stmt = Acquire::make(op->semaphore, op->count, body);
        } else {
            // This semaphore will end up on both sides of the fork,
            // so we'd better duplicate it.
            string cloned_acquire = var->name + unique_name('_');
            cloned_acquires[var->name] = cloned_acquire;
            stmt = Acquire::make(Variable::make(type_of<halide_semaphore_t *>(), cloned_acquire), op->count, body);
        }
    }

    void visit(const Call *op) {
        if (op->name == "halide_semaphore_init") {
            internal_assert(op->args.size() == 2);
            const Variable *var = op->args[0].as<Variable>();
            internal_assert(var);
            inner_semaphores.insert(var->name);
        }
        expr = op;
    }

    map<string, string> &cloned_acquires;
    set<string> inner_semaphores;

public:
    GenerateProducerBody(const string &f, const vector<Expr> &s, map<string, string> &a) :
        func(f), sema(s), cloned_acquires(a) {}
};

class GenerateConsumerBody : public NoOpCollapsingMutator {
    const string &func;
    vector<Expr> sema;

    using NoOpCollapsingMutator::visit;

    void visit(const ProducerConsumer *op) {
        if (op->name == func) {
            if (op->is_producer) {
                // Remove the work entirely
                stmt = Evaluate::make(0);
            } else {
                // Synchronize on the work done by the producer before beginning consumption
                stmt = Acquire::make(sema.back(), 1, op);
                sema.pop_back();
            }
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Acquire *op) {
        // Don't want to duplicate any semaphore acquires. Ones from folding should go to the producer side.
        const Variable *var = op->semaphore.as<Variable>();
        internal_assert(var);
        if (starts_with(var->name, func + ".folding_semaphore.")) {
            stmt = mutate(op->body);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    GenerateConsumerBody(const string &f, const vector<Expr> &s) :
        func(f), sema(s) {}
};

class CloneAcquire : public IRMutator {
    using IRMutator::visit;

    const string &old_name;
    Expr new_var;

    void visit(const Evaluate *op) {
        const Call *call = op->value.as<Call>();
        const Variable *var = ((call && !call->args.empty()) ?
                               call->args[0].as<Variable>() :
                               nullptr);
        if (var && var->name == old_name &&
            (call->name == "halide_semaphore_release" ||
             call->name == "halide_semaphore_init")) {
            vector<Expr> args = call->args;
            args[0] = new_var;
            Stmt new_stmt =
                Evaluate::make(Call::make(call->type, call->name, args, call->call_type));
            stmt = Block::make(op, new_stmt);
        } else {
            stmt = op;
        }
    }

public:
    CloneAcquire(const string &o, const string &new_name) : old_name(o) {
        new_var = Variable::make(type_of<halide_semaphore_t *>(), new_name);
    }
};

class CountConsumeNodes : public IRVisitor {
    const string &func;

    using IRVisitor::visit;

    void visit(const ProducerConsumer *op) {
        if (op->name == func && !op->is_producer) {
            count++;
        }
        IRVisitor::visit(op);
    }
public:
    CountConsumeNodes(const string &f) : func(f) {}
    int count = 0;
};

class ForkAsyncProducers : public IRMutator {
    using IRMutator::visit;

    const map<string, Function> &env;

    map<string, string> cloned_acquires;

    void visit(const Realize *op) {
        auto it = env.find(op->name);
        internal_assert(it != env.end());
        Function f = it->second;
        if (f.schedule().async()) {
            Stmt body = op->body;

            // Make two copies of the body, one which only does the
            // producer, and one which only does the consumer. Inject
            // synchronization to preserve dependencies. Put them in a
            // task-parallel block.

            // Make a semaphore per consume node
            CountConsumeNodes consumes(op->name);
            body.accept(&consumes);

            vector<string> sema_names;
            vector<Expr> sema_vars;
            for (int i = 0; i < consumes.count; i++) {
                sema_names.push_back(op->name + ".semaphore_" + std::to_string(i));
                sema_vars.push_back(Variable::make(Handle(), sema_names.back()));
            }

            // debug(0) << "Body: " << body << "\n\n";

            Stmt producer = GenerateProducerBody(op->name, sema_vars, cloned_acquires).mutate(body);
            Stmt consumer = GenerateConsumerBody(op->name, sema_vars).mutate(body);

            // debug(0) << "Producer: " << producer << "\n\n";
            // debug(0) << "Consumer: " << consumer << "\n\n";

            // Recurse on both sides
            producer = mutate(producer);
            consumer = mutate(consumer);

            // Run them concurrently
            body = Fork::make(producer, consumer);

            for (const string &sema_name : sema_names) {
                // Make a semaphore on the stack
                Expr sema_space = Call::make(type_of<halide_semaphore_t *>(), "halide_make_semaphore",
                                             {0}, Call::Extern);

                // If there's a nested async producer, we may have
                // recursively cloned this semaphore inside the mutation
                // of the producer and consumer.
                auto it = cloned_acquires.find(sema_name);
                if (it != cloned_acquires.end()) {
                    body = CloneAcquire(sema_name, it->second).mutate(body);
                    body = LetStmt::make(it->second, sema_space, body);
                }

                body = LetStmt::make(sema_name, sema_space, body);
            }

            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    ForkAsyncProducers(const map<string, Function> &e) : env(e) {}
};

// Lowers semaphore initialization from a call to
// "halide_make_semaphore" to an alloca followed by a call into the
// runtime to initialize. TODO: what if something crashes before
// releasing a semaphore. Do we need a destructor? The acquire task
// needs to leave the task queue somehow without running. We need a
// destructor that unblocks all waiters somewhere.
class InitializeSemaphores : public IRMutator {
    using IRMutator::visit;

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        if (op->value.type() == type_of<halide_semaphore_t *>()) {
            vector<pair<string, Expr>> lets;
            // Peel off any enclosing lets
            Expr value = op->value;
            while (const Let *l = value.as<Let>()) {
                lets.emplace_back(l->name, l->value);
                value = l->body;
            }
            const Call *call = value.as<Call>();
            if (call && call->name == "halide_make_semaphore") {
                internal_assert(call->args.size() == 1);

                Expr sema_var = Variable::make(type_of<halide_semaphore_t *>(), op->name);
                Expr sema_init = Call::make(Int(32), "halide_semaphore_init",
                                            {sema_var, call->args[0]}, Call::Extern);
                Expr sema_allocate = Call::make(type_of<halide_semaphore_t *>(), Call::alloca,
                                                {(int)sizeof(halide_semaphore_t)}, Call::Intrinsic);
                stmt = Block::make(Evaluate::make(sema_init), body);
                stmt = LetStmt::make(op->name, sema_allocate, stmt);

                // Re-wrap any other lets
                while (lets.size()) {
                    stmt = LetStmt::make(lets.back().first, lets.back().second, stmt);
                }
                return;
            }
        }
        stmt = LetStmt::make(op->name, op->value, body);
    }

    void visit(const Call *op) {
        internal_assert(op->name != "halide_make_semaphore")
            << "Call to halide_make_semaphore in unexpected place\n";
        expr = op;
    }
};

// Tighten the scope of consume nodes as much as possible to avoid needless synchronization.
class TightenConsumeNodes : public IRMutator {
    using IRMutator::visit;

    Stmt make_consume(string name, bool is_producer, Stmt body) {
        if (const LetStmt *let = body.as<LetStmt>()) {
            return LetStmt::make(let->name, let->value, make_consume(name, is_producer, let->body));
        } else if (const Block *block = body.as<Block>()) {
            // Check which sides it's used on
            Scope<int> scope;
            scope.push(name, 0);
            scope.push(name + ".buffer", 0);
            bool first = stmt_uses_vars(block->first, scope);
            bool rest = stmt_uses_vars(block->rest, scope);
            if (first && rest && is_producer) {
                return ProducerConsumer::make(name, is_producer, body);
            } else if (first && rest) {
                return Block::make(make_consume(name, is_producer, block->first),
                                   make_consume(name, is_producer, block->rest));
            } else if (first) {
                return Block::make(make_consume(name, is_producer, block->first), block->rest);
            } else if (rest) {
                return Block::make(block->first, make_consume(name, is_producer, block->rest));
            } else {
                // Used on neither side?!
                return body;
            }
        } else if (const ProducerConsumer *pc = body.as<ProducerConsumer>()) {
            return ProducerConsumer::make(pc->name, pc->is_producer, make_consume(name, is_producer, pc->body));
        } else if (const Realize *r = body.as<Realize>()) {
            return Realize::make(r->name, r->types, r->bounds, r->condition, make_consume(name, is_producer, r->body));
        } else {
            return ProducerConsumer::make(name, is_producer, body);
        }
    }

    void visit(const ProducerConsumer *op) {
        Stmt body = mutate(op->body);
        if (false && op->is_producer) {
            if (op->body.same_as(body)) {
                stmt = op;
            } else {
                stmt = ProducerConsumer::make(op->name, true, body);
            }
        } else {
            stmt = make_consume(op->name, op->is_producer, body);
        }
    }
};

// Broaden the scope of acquire nodes to pack trailing work into the
// same task and to potentially reduce the nesting depth of tasks.
class ExpandAcquireNodes : public IRMutator {
    using IRMutator::visit;

    void visit(const Block *op) {
        Stmt first = mutate(op->first), rest = mutate(op->rest);
        if (const Acquire *a = first.as<Acquire>()) {
            // May as well nest the rest stmt inside the acquire
            // node. It's also blocked on it.
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(Block::make(a->body, op->rest)));
        } else {
            stmt = Block::make(first, rest);
        }
    }

    void visit(const Realize *op) {
        Stmt body = mutate(op->body);
        if (const Acquire *a = body.as<Acquire>()) {
            // Don't do the allocation until we have the
            // semaphore. Reduces peak memory use.
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(Realize::make(op->name, op->types, op->bounds, op->condition, a->body)));
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        }
    }

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        const Acquire *a = body.as<Acquire>();
        if (a &&
            !expr_uses_var(a->semaphore, op->name) &&
            !expr_uses_var(a->count, op->name)) {
            stmt = Acquire::make(a->semaphore, a->count,
                                 LetStmt::make(op->name, op->value, a->body));
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    void visit(const ProducerConsumer *op) {
        Stmt body = mutate(op->body);
        if (const Acquire *a = body.as<Acquire>()) {
            stmt = Acquire::make(a->semaphore, a->count,
                                 mutate(ProducerConsumer::make(op->name, op->is_producer, a->body)));
        } else {
            stmt = ProducerConsumer::make(op->name, op->is_producer, body);
        }
    }
};

class TightenForkNodes : public IRMutator {
    using IRMutator::visit;

    Stmt make_fork(Stmt first, Stmt rest) {
        const LetStmt *lf = first.as<LetStmt>();
        const LetStmt *lr = rest.as<LetStmt>();
        const Realize *rf = first.as<Realize>();
        const Realize *rr = rest.as<Realize>();
        if (lf && lr &&
            lf->name == lr->name &&
            equal(lf->value, lr->value)) {
            return LetStmt::make(lf->name, lf->value, make_fork(lf->body, lr->body));
        } else if (lf && !stmt_uses_var(rest, lf->name)) {
            return LetStmt::make(lf->name, lf->value, make_fork(lf->body, rest));
        } else if (lr && !stmt_uses_var(first, lr->name)) {
            return LetStmt::make(lr->name, lr->value, make_fork(first, lr->body));
        } else if (rf && !stmt_uses_var(rest, rf->name)) {
            return Realize::make(rf->name, rf->types, rf->bounds, rf->condition, make_fork(rf->body, rest));
        } else if (rr && !stmt_uses_var(first, rr->name)) {
            return Realize::make(rr->name, rr->types, rr->bounds, rr->condition, make_fork(first, rr->body));
        } else {
            return Fork::make(first, rest);
        }
    }

    void visit(const Fork *op) {
        bool old_in_fork = in_fork;
        in_fork = true;
        Stmt first = mutate(op->first), rest = mutate(op->rest);
        in_fork = old_in_fork;
        if (is_no_op(first)) {
            stmt = rest;
        } else if (is_no_op(rest)) {
            stmt = first;
        } else {
            stmt = make_fork(first, rest);
        }
    }

    // This is also a good time to nuke any dangling allocations and lets in the fork children.
    void visit(const Realize *op) {
        Stmt body = mutate(op->body);
        if (in_fork && !stmt_uses_var(body, op->name)) {
            stmt = body;
        } else {
            stmt = Realize::make(op->name, op->types, op->bounds, op->condition, body);
        }
    }

    void visit(const LetStmt *op) {
        Stmt body = mutate(op->body);
        if (in_fork && !stmt_uses_var(body, op->name)) {
            stmt = body;
        } else {
            stmt = LetStmt::make(op->name, op->value, body);
        }
    }

    bool in_fork = false;
};

// TODO: merge semaphores

Stmt fork_async_producers(Stmt s, const map<string, Function> &env) {
    s = TightenConsumeNodes().mutate(s);
    s = ForkAsyncProducers(env).mutate(s);
    s = ExpandAcquireNodes().mutate(s);
    s = TightenForkNodes().mutate(s);
    s = InitializeSemaphores().mutate(s);
    return s;
}

}
}