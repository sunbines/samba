#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <inttypes.h>

#include <popt.h>
#include <netapi.h>

#include "common.h"

void popt_common_callback(poptContext con,
			 enum poptCallbackReason reason,
			 const struct poptOption *opt,
			 const char *arg, const void *data)
{
	struct libnetapi_ctx *ctx = NULL;

	libnetapi_getctx(&ctx);

	if (reason == POPT_CALLBACK_REASON_PRE) {
	}

	if (reason == POPT_CALLBACK_REASON_POST) {
	}

	if (!opt) {
		return;
	}
	switch (opt->val) {
		case 'U': {
			char *puser = strdup(arg);
			char *p = NULL;

			if ((p = strchr(puser,'%'))) {
				size_t len;
				*p = 0;
				libnetapi_set_username(ctx, puser);
				libnetapi_set_password(ctx, p+1);
				len = strlen(p+1);
				memset(strchr(arg,'%')+1,'X',len);
			} else {
				libnetapi_set_username(ctx, puser);
			}
			free(puser);
			break;
		}
		case 'd':
			libnetapi_set_debuglevel(ctx, arg);
			break;
		case 'p':
			libnetapi_set_password(ctx, arg);
			break;
		case 'k':
			libnetapi_set_use_kerberos(ctx);
			break;
	}
}

struct poptOption popt_common_netapi_examples[] = {
	{
		.argInfo = POPT_ARG_CALLBACK|POPT_CBFLAG_PRE|POPT_CBFLAG_POST,
		.arg = (void *)popt_common_callback,
	},
	{
		.longName   = "user",
		.shortName  = 'U',
		.argInfo    = POPT_ARG_STRING,
		.val        = 'U',
		.descrip    = "Username used for connection",
		.argDescrip = "USERNAME",
	},
	{
		.longName   = "password",
		.shortName  = 'p',
		.argInfo    = POPT_ARG_STRING,
		.val        = 'p',
		.descrip    = "Password used for connection",
		.argDescrip = "PASSWORD",
	},
	{
		.longName   = "debuglevel",
		.shortName  = 'd',
		.argInfo    = POPT_ARG_STRING,
		.val        = 'd',
		.descrip    = "Debuglevel",
		.argDescrip = "DEBUGLEVEL",
	},
	{
		.longName   = "kerberos",
		.shortName  = 'k',
		.argInfo    = POPT_ARG_NONE,
		.val        = 'k',
		.descrip    = "Use Kerberos",
	},
	POPT_TABLEEND
};