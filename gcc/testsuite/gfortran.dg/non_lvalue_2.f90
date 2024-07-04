! { dg-do compile }
! { dg-additional-options "-fdump-tree-original" }
!
! Check the removal of NON_LVALUE_EXPR if they are used in a non-lvalue context

! The NON_LVALUE_EXPR is dropped if it's part (left operand) of a bigger expression
function f1 (f1_arg1, f1_arg2)
  integer, value :: f1_arg1, f1_arg2
  integer :: f1
  f1 = (f1_arg1 + 0) + f1_arg2
end function
! { dg-final { scan-tree-dump "__result_f1 = f1_arg1 \\+ f1_arg2;" "original" } }

! The NON_LVALUE_EXPR is dropped if it's part (right operand) of a bigger expression
function f2 (f2_arg1, f2_arg2)
  integer, value :: f2_arg1, f2_arg2
  integer :: f2
  f2 = f2_arg1 + (f2_arg2 + 0)
end function
! { dg-final { scan-tree-dump "__result_f2 = f2_arg1 \\+ f2_arg2;" "original" } }

! The NON_LVALUE_EXPR is dropped if it's part (left operand) of a binary logical operator
function f3 (f3_arg1)
  integer, value :: f3_arg1
  logical :: f3
  f3 = (f3_arg1 + 0) > 0
end function
! { dg-final { scan-tree-dump "__result_f3 = f3_arg1 > 0;" "original" } }

! The NON_LVALUE_EXPR is dropped if it's part (right operand) of a binary logical operator
function f4 (f4_arg1, f4_arg2)
  integer, value :: f4_arg1, f4_arg2
  logical :: f4
  f4 = f4_arg1 > (f4_arg2 + 0)
end function
! { dg-final { scan-tree-dump "__result_f4 = f4_arg1 > f4_arg2;" "original" } }

! The NON_LVALUE_EXPR is dropped if it's part of a unary operator
function f5 (f5_arg1)
  integer, value :: f5_arg1
  integer :: f5
  f5 = -(not(not(f5_arg1)))
end function
! { dg-final { scan-tree-dump "__result_f5 = -f5_arg1;" "original" } }
