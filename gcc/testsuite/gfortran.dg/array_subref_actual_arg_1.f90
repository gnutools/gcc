! { dg-do run }
! { dg-additional-options "-Warray-temporaries -fdump-tree-original" }
!
! Check correct passing of subreference arrays, with either a descriptor
! without data copy to a temporary, or no descriptor and data copy.
! 
! We check the presence of temporaries in the dump based on the variable name
! array descriptors that don't use a temporary are named PARM, whereas variables
! that do data copy are named ATMP.

module m

  implicit none
  integer, parameter :: k = selected_int_kind (6)
  type :: t
    integer(kind=k) :: a, b
  end type t
  type, extends(t) :: u
    integer(kind=k) :: c
  end type u
  integer, parameter :: s = 3 ! number of integers in a type u 
  integer, parameter :: r = 3 ! extent of x in each dimension
  type(u) :: x(r, r)
  integer, parameter :: dat(s*r*r) =  (/  2,  3,  5,  7, 11, 13,  &
                                         17, 19, 23, 29, 31, 37,  &
                                         41, 43, 47, 53, 59, 61,  &
                                         67, 71, 73, 79, 83, 89,  &
                                         97,101,103  /)

contains

  subroutine init(z)
    type(u) :: z(:,:)
    integer :: i, j
    do j=1,r
      associate (n => (j-1)*r*s)
        do i = 1,r
          associate(m => n+(i-1)*s)
            associate(p => dat(m+1:m+s))
              z(i,j) = u(p(1), p(2), p(3))
            end associate
          end associate
        end do
      end associate
    end do
  end subroutine init

  subroutine check(z, i, j, p1, p2, error_code)
    type(t), intent(in) :: z
    integer, intent(in) :: i, j,  error_code
    integer(kind=k), intent(in) :: p1, p2
    if (z%a /= p1 .or. z%b /= p2) then
      print *, i, j
      print *, z
      print *, p1, p2
      error stop error_code
    end if
  end subroutine check

  subroutine sub_assumed_shape(y)
    type(t), intent(in) :: y(:,:)
    integer :: i, j
    if (any(shape(y) /= shape(x))) error stop 1
    do j=1,r
      associate (n => (j-1)*r*s)
        do i = 1,r
          associate(m => n+(i-1)*s)
            associate(p => dat(m+1:m+s))
              call check(y(i,j), i, j, p(1), p(2), 2)
            end associate
          end associate
        end do
      end associate
    end do
  end subroutine sub_assumed_shape

  subroutine sub_explicit(y)
    type(t), intent(in) :: y(r,r)
    integer :: i, j
    if (any(shape(y) /= shape(x))) error stop 11
    do j=1,r
      associate (n => (j-1)*r*s)
        do i = 1,r
          associate(m => n+(i-1)*s)
            associate(p => dat(m+1:m+s))
              call check(y(i, j), i, j, p(1), p(2), 12)
            end associate
          end associate
        end do
      end associate
    end do
  end subroutine sub_explicit

  subroutine sub_assumed_size(y)
    type(t), intent(in) :: y(r,*)
    integer :: i, j
    if (size(y,1) /= size(x,1)) error stop 21
    do j=1,r
      associate (n => (j-1)*r*s)
        do i = 1,r
          associate(m => n+(i-1)*s)
            associate(p => dat(m+1:m+s))
              call check(y(i, j), i, j, p(1), p(2), 22)
            end associate
          end associate
        end do
      end associate
    end do
  end subroutine sub_assumed_size

  subroutine sub_assumed_rank(y)
    type(t), intent(in) :: y(..)
    integer :: i, j
    if (any(shape(y) /= shape(x))) error stop 41
    select rank (y)
      rank(2)
        do j=1,r
          associate (n => (j-1)*r*s)
            do i = 1,r
              associate(m => n+(i-1)*s)
                associate(p => dat(m+1:m+s))
                  call check(y(i, j), i, j, p(1), p(2), 42)
                end associate
              end associate
            end do
          end associate
        end do
      rank default
        error stop 43
    end select
  end subroutine sub_assumed_rank

end module m

subroutine sub_implicit(y)
  use m
  type(t), intent(in) :: y(r,r)
  integer :: i, j
  if (size(y,1) /= size(x,1)) error stop 31
  do j=1,r
    associate (n => (j-1)*r*s)
      do i = 1,r
        associate(m => n+(i-1)*s)
          associate(p => dat(m+1:m+s))
            call check(y(i, j), i, j, p(1), p(2), 32)
          end associate
        end associate
      end do
    end associate
  end do
end subroutine sub_implicit

program p
  use m
  implicit none

  call init(x)

  ! Descriptor without data copy: one single usage of the data pointer for its initialisation.
  call sub_assumed_shape(x%t)
  ! { dg-final { scan-tree-dump-var {sub_assumed_shape \(&parm\.(\d+)\);} original ashp_parm_id } }
  ! { dg-final { global ashp_parm_id; scan-tree-dump-times "parm.${ashp_parm_id}\\.data" 1 original } }

  ! Use a temporary; there are three usages of the data pointer: one for its initialisation,
  ! one for the data copy, and one for passing as actual argument
  call sub_explicit(x%t)       ! { dg-warning "array temporary" }
  ! { dg-final { scan-tree-dump-var {sub_explicit \(\(.*?\) atmp.(\d+)\.data\);} original expl_tmp_id } }
  ! { dg-final { global expl_tmp_id; scan-tree-dump-times "atmp.${expl_tmp_id}\\.data" 3 original } }

  ! Use a temporary; there are three usages of the data pointer: one for its initialisation,
  ! one for the data copy, and one for passing as actual argument
  call sub_assumed_size(x%t)   ! { dg-warning "array temporary" }
  ! { dg-final { scan-tree-dump-var {sub_assumed_size \(\(.*?\) atmp.(\d+)\.data\);} original asz_tmp_id } }
  ! { dg-final { global asz_tmp_id; scan-tree-dump-times "atmp.${asz_tmp_id}\\.data" 3 original } }

  ! Use a temporary; there are four usages of the data pointer: one for its initialisation,
  ! one for the data copy in, one for passing as actual argument, and one for data copy out
  call sub_implicit(x%t)       ! { dg-warning "array temporary" }
  ! { dg-final { scan-tree-dump-var {sub_implicit \(\(.*?\) atmp.(\d+)\.data\);} original impl_tmp_id } }
  ! { dg-final { global impl_tmp_id; scan-tree-dump-times "atmp.${impl_tmp_id}\\.data" 4 original } }

  ! Descriptor without data copy: one single usage of the data pointer for its initialisation.
  call sub_assumed_rank(x%t)
  ! { dg-final { scan-tree-dump-var {sub_assumed_rank \(&parm\.(\d+)\);} original arnk_parm_id } }
  ! { dg-final { global arnk_parm_id; scan-tree-dump-times "parm.${arnk_parm_id}\\.data" 1 original } }
end program p
