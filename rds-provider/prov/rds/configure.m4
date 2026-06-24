dnl Configury specific to the libfabric RDS provider
dnl
dnl Called to configure this provider
dnl
dnl Arguments:
dnl
dnl $1: action if configured successfully
dnl $2: action if not configured successfully
dnl
AC_DEFUN([FI_RDS_CONFIGURE],[
	# Determine if we can support the rds provider
	rds_happy=0
	AS_IF([test x"$enable_rds" != x"no"],
	      [
	       dnl The provider builds directly on the kernel RDS UAPI and BSD
	       dnl sockets.  <linux/rds.h> carries the rds_rdma_args /
	       dnl rds_get_mr_args structs and the rds_rdma_cookie_t type.
	       AC_CHECK_HEADER([sys/socket.h],
	                       [rds_h_happy=1], [rds_h_happy=0])
	       AC_CHECK_HEADER([linux/rds.h],
	                       [rds_rds_h_happy=1], [rds_rds_h_happy=0])

	       dnl rds_rdma_args.user_token is required for asynchronous RMA
	       dnl completion (RDS_CMSG_RDMA_STATUS -> rds_rdma_notify).  Newer
	       dnl kernels have it; warn loudly if the field is missing.
	       AC_CHECK_MEMBER([struct rds_rdma_args.user_token],
	                       [rds_token_happy=1], [rds_token_happy=0],
	                       [[#include <linux/rds.h>]])

	       AS_IF([test $rds_h_happy -eq 1 -a $rds_rds_h_happy -eq 1],
	             [rds_happy=1], [rds_happy=0])

	       AS_IF([test $rds_happy -eq 1 -a $rds_token_happy -eq 0],
	             [AC_MSG_WARN([struct rds_rdma_args has no user_token member; dnl
the RDS provider needs a kernel whose UAPI exposes it for RMA completions.])])
	      ])

	AS_IF([test $rds_happy -eq 1], [$1], [$2])
])
