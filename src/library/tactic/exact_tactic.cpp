/*
Copyright (c) 2014 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "kernel/type_checker.h"
#include "kernel/for_each_fn.h"
#include "kernel/error_msgs.h"
#include "library/util.h"
#include "library/constants.h"
#include "library/reducible.h"
#include "library/tactic/tactic.h"
#include "library/tactic/elaborate.h"
#include "library/tactic/expr_to_tactic.h"

namespace lean {
// Return true iff \c e is of the form (?m l_1 ... l_n), where ?m is a metavariable and l_i's local constants
bool is_meta_placeholder(expr const & e) {
    if (!is_meta(e))
        return false;
    buffer<expr> args;
    get_app_args(e, args);
    return std::all_of(args.begin(), args.end(), is_local);
}

tactic exact_tactic(elaborate_fn const & elab, expr const & e, bool enforce_type_during_elaboration, bool allow_metavars,
                    bool conservative) {
    return tactic01([=](environment const & env, io_state const & ios, proof_state const & s) {
            proof_state new_s = s;
            goals const & gs  = new_s.get_goals();
            if (!gs) {
                throw_no_goal_if_enabled(s);
                return none_proof_state();
            }
            expr t                 = head(gs).get_type();
            bool report_unassigned = !allow_metavars && enforce_type_during_elaboration && s.report_failure();
            optional<expr> new_e;
            try {
                new_e = elaborate_with_respect_to(env, ios, elab, new_s, e, some_expr(t),
                                                  report_unassigned, enforce_type_during_elaboration,
                                                  conservative);
            } catch (exception &) {
                if (s.report_failure())
                    throw;
                else
                    return none_proof_state();
            }
            if (new_e) {
                goals const & gs   = new_s.get_goals();
                if (gs) {
                    goal const & g     = head(gs);
                    if (!allow_metavars && has_expr_metavar_relaxed(*new_e)) {
                        throw_tactic_exception_if_enabled(s, [=](formatter const & fmt) {
                                format r = format("invalid 'exact' tactic, term still contains metavariables "
                                                  "after elaboration");
                                r       += pp_indent_expr(fmt, *new_e);
                                return r;
                            });
                        return none_proof_state();
                    }
                    substitution subst = new_s.get_subst();
                    assign(subst, g, *new_e);
                    if (allow_metavars) {
                        buffer<goal> new_goals;
                        name_generator ngen = new_s.get_ngen();
                        auto tc             = mk_type_checker(env, ngen.mk_child());
                        for_each(*new_e, [&](expr const & m, unsigned) {
                                if (!has_expr_metavar(m))
                                    return false;
                                if (is_meta_placeholder(m)) {
                                    new_goals.push_back(goal(m, tc->infer(m).first));
                                    return false;
                                }
                                return !is_metavar(m) && !is_local(m);
                            });
                        goals new_gs = to_list(new_goals.begin(), new_goals.end(), tail(gs));
                        return some(proof_state(new_s, new_gs, subst, ngen));
                    } else {
                        return some(proof_state(new_s, tail(gs), subst));
                    }
                } else {
                    return some_proof_state(new_s);
                }
            }
            return none_proof_state();
        });
}

static tactic assumption_tactic_core(bool conservative) {
    return tactic([=](environment const & env, io_state const & ios, proof_state const & s) {
            goals const & gs = s.get_goals();
            if (empty(gs)) {
                throw_no_goal_if_enabled(s);
                return proof_state_seq();
            }
            proof_state new_s = s.update_report_failure(false);
            optional<tactic> tac;
            goal g = head(gs);
            buffer<expr> hs;
            g.get_hyps(hs);
            auto elab = [](goal const &, name_generator const &, expr const & H,
                           optional<expr> const &, substitution const & s, bool) -> elaborate_result {
                return elaborate_result(H, s, constraints());
            };
            unsigned i = hs.size();
            while (i > 0) {
                --i;
                expr const & h = hs[i];
                tactic curr = exact_tactic(elab, h, false, false, conservative);
                if (tac) {
                    if (conservative)
                        tac = orelse(*tac, curr);
                    else
                        tac = append(*tac, curr);
                } else {
                    tac = curr;
                }
            }
            if (tac) {
                return (*tac)(env, ios, s);
            } else {
                return proof_state_seq();
            }
        });
}

tactic eassumption_tactic() {
    return assumption_tactic_core(false);
}

tactic assumption_tactic() {
    return assumption_tactic_core(true);
}

static expr * g_exact_tac_fn   = nullptr;
static expr * g_rexact_tac_fn  = nullptr;
static expr * g_refine_tac_fn  = nullptr;
expr const & get_exact_tac_fn()  { return *g_exact_tac_fn; }
expr const & get_rexact_tac_fn() { return *g_rexact_tac_fn; }
expr const & get_refine_tac_fn() { return *g_refine_tac_fn; }
void initialize_exact_tactic() {
    name const & exact_tac_name  = get_tactic_exact_name();
    name const & rexact_tac_name = get_tactic_rexact_name();
    name const & refine_tac_name = get_tactic_refine_name();
    g_exact_tac_fn  = new expr(Const(exact_tac_name));
    g_rexact_tac_fn = new expr(Const(rexact_tac_name));
    g_refine_tac_fn = new expr(Const(refine_tac_name));
    register_tac(exact_tac_name,
                 [](type_checker &, elaborate_fn const & fn, expr const & e, pos_info_provider const *) {
                     check_tactic_expr(app_arg(e), "invalid 'exact' tactic, invalid argument");
                     return exact_tactic(fn, get_tactic_expr_expr(app_arg(e)), true, false, false);
                 });
    register_tac(rexact_tac_name,
                 [](type_checker &, elaborate_fn const & fn, expr const & e, pos_info_provider const *) {
                     check_tactic_expr(app_arg(e), "invalid 'rexact' tactic, invalid argument");
                     return exact_tactic(fn, get_tactic_expr_expr(app_arg(e)), false, false, false);
                 });
    register_tac(refine_tac_name,
                 [](type_checker &, elaborate_fn const & fn, expr const & e, pos_info_provider const *) {
                     check_tactic_expr(app_arg(e), "invalid 'refine' tactic, invalid argument");
                     return exact_tactic(fn, get_tactic_expr_expr(app_arg(e)), true, true, false);
                 });
    register_simple_tac(get_tactic_eassumption_name(),
                        []() { return eassumption_tactic(); });

    register_simple_tac(get_tactic_assumption_name(),
                        []() { return assumption_tactic(); });
}
void finalize_exact_tactic() {
    delete g_exact_tac_fn;
    delete g_rexact_tac_fn;
    delete g_refine_tac_fn;
}
}
