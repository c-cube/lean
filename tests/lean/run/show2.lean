import data.nat
open nat

example : ∀ a b, a + b = b + a :=
show ∀ a b : nat, a + b = b + a
| 0        0        := rfl
| 0        (succ b) := by rewrite zero_add
| (succ a) 0        := by rewrite zero_add
| (succ a) (succ b) := by rewrite [succ_add, this]
