! { dg-additional-options -Wuninitialized }

type t4
  integer, allocatable :: quux(:)
end type t4
type t3
  type(t4), pointer :: qux(:)
end type t3
type t2
  type(t3), allocatable :: bar(:)
end type t2
type t
  type(t2), allocatable :: foo(:)
end type t

type(t), allocatable :: c(:)
! { dg-note {'c' declared here} {} { target *-*-* } .-1 }
! { dg-note {'c.offset' was declared here} {} { target *-*-* } .-2 }
! { dg-note {'c.span' was declared here} {} { target *-*-* } .-3 }

!$acc enter data copyin(c(5)%foo(4)%bar(3)%qux(2)%quux(:))
! { dg-warning {'c\.offset' is used uninitialized} {} { target *-*-* } .-1 }
! { dg-warning {'c\.span' is used uninitialized} {} { target *-*-* } .-2 }
!$acc exit data delete(c(5)%foo(4)%bar(3)%qux(2)%quux(:))
! { dg-warning {'c\.offset' is used uninitialized} {} { target *-*-* } .-1 }
! { dg-warning {'c\.span' is used uninitialized} {} { target *-*-* } .-2 }
end
