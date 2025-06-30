! { dg-do run }
! { dg-options "-fdump-tree-original" }
!
! PR fortran/48820
!
! Ensure that the value of scalars to assumed-rank arrays is
! copied back - and everything happens in the correct order.

call sub(f())
contains
subroutine sub(x)
  integer, pointer :: x(..)
end subroutine sub
function f() result(res)
  integer, pointer :: res
end function f
end

! { dg-final { scan-tree-dump " = f \\(\\);" "original" } }
! { dg-final { scan-tree-dump "desc.0.dtype = .*;" "original" } }
! { dg-final { scan-tree-dump "desc.0.data = .void .. D.*;" "original" } }
! { dg-final { scan-tree-dump "sub \\(&desc.0\\);" "original" } }
! { dg-final { scan-tree-dump "D.*= .integer.kind=4. .. desc.0.data;" "original" } }

