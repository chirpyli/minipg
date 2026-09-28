/*-----------------------------------------------------------------------
 *
 * PostgreSQL locale utilities
 *
 * Portions Copyright (c) 2002-2021, PostgreSQL Global Development Group
 *
 * src/backend/utils/adt/pg_locale.c
 *
 *-----------------------------------------------------------------------
 */

/*----------
 * minipg 已裁剪 locale 机制：
 *
 * 进程不再吸收环境或 pg_database 中的 locale，LC_COLLATE/LC_CTYPE 恒为 C，
 * LC_MESSAGES/LC_NUMERIC 已无消费者；per-object collation 也已移除，所有
 * 排序规则一律按 C 处理。因此运行期排序和大小写转换都走字节序/ASCII
 * 路径，不再依赖 strcoll()/strxfrm()。
 *
 * !!! NOW HEAR THIS !!!
 *
 * We've been bitten repeatedly by this bug, so let's try to keep it in
 * mind in future: on some platforms, the locale functions return pointers
 * to static data that will be overwritten by any later locale function.
 * Thus, for example, the obvious-looking sequence
 *			save = setlocale(category, NULL);
 *			if (!setlocale(category, value))
 *				fail = true;
 *			setlocale(category, save);
 * DOES NOT WORK RELIABLY: on some platforms the second setlocale() call
 * will change the memory save is pointing at.  To do this sort of thing
 * safely, you *must* pstrdup what setlocale returns the first time.
 *
 * The POSIX locale standard is available here:
 *
 *	http://www.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap07.html
 *----------
 */


#include "postgres.h"

#include "catalog/pg_collation.h"
#include "catalog/pg_control.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"


/*
 * Detect whether collation's LC_COLLATE property is C
 */
bool
lc_collate_is_c(Oid collation)
{
	/*
	 * minipg 已裁剪 locale 与 collation 概念：进程恒为 C locale，
	 * pg_collation 系统表也已移除，所有有效 collation 一律按 C（字节序）
	 * 处理，因此这里无需查询任何 catalog，也与进程继承的 LC_COLLATE
	 * 环境无关。这保证索引构建可重现。
	 *
	 * 对 "collation 0"（非法值）返回 false，让调用方走非 C 路径报出排序
	 * 规则非法。
	 */
	return OidIsValid(collation);
}

/*
 * Detect whether collation's LC_CTYPE property is C
 */
bool
lc_ctype_is_c(Oid collation)
{
	/*
	 * 同上：进程恒为 C locale，upper/lower/initcap 等一律走单字节（ASCII）
	 * 路径，与进程继承的 locale 环境无关。
	 */
	return OidIsValid(collation);
}


/*
 * Create a locale_t from a collation OID.
 *
 * minipg 只有 C 排序规则（见文件头说明）：pg_collation 系统表已移除，
 * 排序与大小写转换一律走字节序/ASCII 路径，因此这里恒返回 0，表示
 * "走不带 locale_t 的代码路径"。保留该接口是为了让 varstr_cmp()、
 * wchar2char() 等调用点保持与 PostgreSQL 相同的结构。
 */
pg_locale_t
pg_newlocale_from_collation(Oid collid)
{
	/* Callers must pass a valid OID */
	Assert(OidIsValid(collid));

	return (pg_locale_t) 0;
}

/*
 * These functions convert from/to libc's wchar_t, *not* pg_wchar_t.
 * Therefore we keep them here rather than with the mbutils code.
 */

/*
 * wchar2char --- convert wide characters to multibyte format
 *
 * This has the same API as the standard wcstombs_l() function; in particular,
 * tolen is the maximum number of bytes to store at *to, and *from must be
 * zero-terminated.  The output will be zero-terminated iff there is room.
 */
size_t
wchar2char(char *to, const wchar_t *from, size_t tolen, pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

	if (locale == (pg_locale_t) 0)
	{
		/* Use wcstombs directly for the default locale */
		result = wcstombs(to, from, tolen);
	}
	else
	{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_WCSTOMBS_L
		/* Use wcstombs_l for nondefault locales */
		result = wcstombs_l(to, from, tolen, locale->info.lt);
#else							/* !HAVE_WCSTOMBS_L */
		/* We have to temporarily set the locale as current ... ugh */
		locale_t	save_locale = uselocale(locale->info.lt);

		result = wcstombs(to, from, tolen);

		uselocale(save_locale);
#endif							/* HAVE_WCSTOMBS_L */
#else							/* !HAVE_LOCALE_T */
		/* Can't have locale != 0 without HAVE_LOCALE_T */
		elog(ERROR, "wcstombs_l is not available");
		result = 0;				/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
	}

	return result;
}

/*
 * char2wchar --- convert multibyte characters to wide characters
 *
 * This has almost the API of mbstowcs_l(), except that *from need not be
 * null-terminated; instead, the number of input bytes is specified as
 * fromlen.  Also, we ereport() rather than returning -1 for invalid
 * input encoding.  tolen is the maximum number of wchar_t's to store at *to.
 * The output will be zero-terminated iff there is room.
 */
size_t
char2wchar(wchar_t *to, size_t tolen, const char *from, size_t fromlen,
		   pg_locale_t locale)
{
	size_t		result;

	Assert(!locale || locale->provider == COLLPROVIDER_LIBC);

	if (tolen == 0)
		return 0;

	{
		/* mbstowcs requires ending '\0' */
		char	   *str = pnstrdup(from, fromlen);

		if (locale == (pg_locale_t) 0)
		{
			/* Use mbstowcs directly for the default locale */
			result = mbstowcs(to, str, tolen);
		}
		else
		{
#ifdef HAVE_LOCALE_T
#ifdef HAVE_MBSTOWCS_L
			/* Use mbstowcs_l for nondefault locales */
			result = mbstowcs_l(to, str, tolen, locale->info.lt);
#else							/* !HAVE_MBSTOWCS_L */
			/* We have to temporarily set the locale as current ... ugh */
			locale_t	save_locale = uselocale(locale->info.lt);

			result = mbstowcs(to, str, tolen);

			uselocale(save_locale);
#endif							/* HAVE_MBSTOWCS_L */
#else							/* !HAVE_LOCALE_T */
			/* Can't have locale != 0 without HAVE_LOCALE_T */
			elog(ERROR, "mbstowcs_l is not available");
			result = 0;			/* keep compiler quiet */
#endif							/* HAVE_LOCALE_T */
		}

		pfree(str);
	}

	if (result == -1)
	{
		/*
		 * Invalid multibyte character encountered.  We try to give a useful
		 * error message by letting pg_verifymbstr check the string.  But it's
		 * possible that the string is OK to us, and not OK to mbstowcs ---
		 * this suggests that the LC_CTYPE locale is different from the
		 * database encoding.  Give a generic error message if pg_verifymbstr
		 * can't find anything wrong.
		 */
		pg_verifymbstr(from, fromlen, false);	/* might not return */
		/* but if it does ... */
		ereport(ERROR,
				(errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
				 errmsg("invalid multibyte character for locale"),
				 errhint("The server's LC_CTYPE locale is probably incompatible with the database encoding.")));
	}

	return result;
}
