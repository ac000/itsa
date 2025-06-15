/* SPDX-License-Identifier: GPL-2.0 */

/*
 * itsa.c - Provide Income TAX Self-Assessment via UK's HMRC MTD API
 *
 * Copyright (c) 2021 - 2025	 Andrew Clayton <ac@sigsegv.uk>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <regex.h>
#include <limits.h>

#include <sqlite3.h>

#include <jansson.h>

#include <libmtdac/mtd.h>

#include <libac.h>

#include "platform.h"
#include "color.h"

#define PROD_NAME		"itsa"

#define ITSA_CFG		".config/itsa/config.json"
#define DEFAULT_EDITOR		"vi"

#define list_for_each(cur, list)	for (cur = list; cur; cur = cur->next)

#define MSG_INFO		"#HI_BLUE#INFO#RST#"
#define MSG_WARN		"#HI_YELLOW#WARNINGS#RST#"
#define MSG_ERR			"#HI_RED#ERRORS#RST#"

#define STRUE			"#HI_GREEN#t#RST#"
#define SFALSE			"#HI_RED#f#RST#"

#define TAX_YEAR_SZ		7

#define INFO	"[#INFO#INFO#RST#] "
#define FINAL_DECLARATION \
INFO "Before you can submit the information displayed here in response\n"\
INFO "to your notice to file from HM Revenue & Customs, you must read\n"\
INFO "and agree to the following statement by selecting (y).\n"\
"\n"\
INFO "I declare that the information and self-assessment I have filed are\n"\
INFO "(taken together) correct and complete to the best of my knowledge.\n"\
INFO "I understand that I may have to pay financial penalties and face\n"\
INFO "prosecution if I give false information.\n"\
"\n"\
INFO "By saying yes (y) below, you are declaring that you agree with\n"\
INFO "the above and wish to proceed with final crystallisation.\n"

static struct {
	const char *gnc;
	const char *bid;
	const char *bname;
	const char *btype;
} itsa_config;
#define BUSINESS_ID	itsa_config.bid
#define BUSINESS_NAME	itsa_config.bname
#define BUSINESS_TYPE	itsa_config.btype

static char const *extra_hdrs[5];

static bool is_prod_api;

static int JKEY_FW;

static void disp_usage(void)
{
	printf("Usage: itsa COMMAND [OPTIONS]\n\n");
	printf("Commands\n");
	printf("    init\n");
	printf("    re-auth\n");
	printf("\n");
	printf("    switch-business\n");
	printf("\n");
	printf("    list-periods [<start> <end>]\n");
	printf("    create-period <tax_year> [<start> <end>]\n");
	printf("    update-period <tax_year> <period_id>\n");
	printf("    update-annual-summary <tax_year>\n");
	printf("    get-end-of-period-statement-obligations [<start> <end>]\n");
	printf("    submit-final-declaration <tax_year>\n");
	printf("    list-calculations <tax_year> [calculation_type]\n");
	printf("    view-end-of-year-estimate\n");
	printf("    add-savings-account\n");
	printf("    view-savings-accounts [tax_year]\n");
	printf("    amend-savings-account <tax_year>\n");
}

static void free_config(void)
{
	free((void *)itsa_config.gnc);
	free((void *)itsa_config.bid);
	free((void *)itsa_config.bname);
	free((void *)itsa_config.btype);
}

#define __cleanup_free	__attribute__((cleanup(xfree)))
static inline void xfree(char **p)
{
	free(*p);
}

/*
 * Simple wrapper around time(2) that allows to override the
 * current date.
 */
static time_t xtime(void)
{
	const char *set_date = getenv("ITSA_SET_DATE");
	struct tm tm = {};

	if (!set_date)
		return time(NULL);

	strptime(set_date, "%F", &tm);

	return mktime(&tm);
}

static json_t *get_result_json(const char *buf)
{
	json_t *jarray;
	json_t *root;
	json_t *result;

	jarray = json_loads(buf, 0, NULL);
	root = json_array_get(jarray, json_array_size(jarray) - 1);
	result = json_deep_copy(json_object_get(root, "result"));
	json_decref(jarray);

	return result;
}

/*
 * For doing request back-off, following the Fibonaci Sequence
 * (skipping 0)
 */
static int next_fib(int last)
{
	int Fn;
	static int state;

	if (last == -1) {
		state = 0;
		return 1;
	}

	Fn = state + last;
	state = last;

	return Fn;
}

static const char *get_period_color(const char *start, const char *end,
				    const char *due, bool met)
{
	time_t now = xtime();
	time_t st;
	time_t et;
	time_t dt;
	struct tm stm = {};
	struct tm etm = {};
	struct tm dtm = {};

	strptime(start, "%F", &stm);
	strptime(end, "%F", &etm);
	strptime(due, "%F", &dtm);

	st = mktime(&stm);
	/*
	 * add 86399 seconds onto the date/time to make it
	 * 23:59:59 on the day in question. Lets ignore leap
	 * seconds for now...
	 */
	et = mktime(&etm) + 86400 - 1;
	dt = mktime(&dtm) + 86400 - 1;

	if (met && now > dt)
		return "#GREEN#";
	if (now > et && now <= dt)
		return "#TANG#";
	if (now >= st && now <= et)
		return "";
	if (!met && now > dt)
		return "#RED#";

	return "#CHARC#";
}

static char *get_tax_year(const char *date, char *buf)
{
	struct tm tm = {};
	char year[5];
	char year2[5];

	if (!date) {
		time_t now = xtime();
		struct tm *ntm;

		ntm = localtime(&now);
		memcpy(&tm, ntm, sizeof(struct tm));
	} else {
		strptime(date, "%F", &tm);
	}
	strftime(year, sizeof(year), "%Y", &tm);

	/* tm_mon starts at 0, hence April = 3 */
	if (tm.tm_mon < 3 || (tm.tm_mon <= 3 && tm.tm_mday <= 5)) {
		tm.tm_year--;
		strftime(year2, sizeof(year2), "%Y", &tm);
		snprintf(buf, 8, "%s-%s", year2, year + 2);
	} else {
		tm.tm_year++;
		strftime(year2, sizeof(year2), "%Y", &tm);
		snprintf(buf, 8, "%s-%s", year, year2 + 2);
	}

	return buf;
}

/*
 * transactions.guid	-> splits.tx_guid	: Item value
 * splits.account_guid	-> accounts.guid	: Account type (in/out)
 */
static void get_data(const char *start, const char *end, long *income,
		     long *expenses)
{
	sqlite3_stmt *trans_stmt;
	sqlite3_stmt *splits_stmt;
	sqlite3_stmt *acc_stmt;
	sqlite3 *db;
	char sql[512];
	ac_slist_t *i_list = NULL;
	ac_slist_t *e_list = NULL;
	ac_slist_t *p;

	*income = *expenses = 0;

	sqlite3_open(itsa_config.gnc, &db);
	snprintf(sql, sizeof(sql),
		 "SELECT * FROM transactions WHERE "
		 "post_date >= ? AND post_date <= ?");
	sqlite3_prepare_v2(db, sql, -1, &trans_stmt, NULL);
	sqlite3_bind_text(trans_stmt, 1, start, strlen(start), SQLITE_STATIC);
	sqlite3_bind_text(trans_stmt, 2, end, strlen(end), SQLITE_STATIC);

	snprintf(sql, sizeof(sql),
		 "SELECT value_num, account_guid FROM splits WHERE "
		 "tx_guid = ? AND value_num > 0 LIMIT 1");
	sqlite3_prepare_v2(db, sql, -1, &splits_stmt, NULL);

	snprintf(sql, sizeof(sql),
		 "SELECT account_type FROM accounts WHERE guid = ?");
	sqlite3_prepare_v2(db, sql, -1, &acc_stmt, NULL);

	while (sqlite3_step(trans_stmt) == SQLITE_ROW) {
		char *item;
		const char *account;
		const unsigned char *date = sqlite3_column_text(trans_stmt, 3);
		const unsigned char *desc = sqlite3_column_text(trans_stmt, 5);
		const unsigned char *tx_guid =
			sqlite3_column_text(trans_stmt, 0);
		const unsigned char *account_guid;
		long amnt;
		int len;

		sqlite3_bind_text(splits_stmt, 1, (const char *)tx_guid,
				  sqlite3_column_bytes(trans_stmt, 0),
				  SQLITE_STATIC);
		sqlite3_step(splits_stmt);

		amnt = sqlite3_column_int(splits_stmt, 0);
		account_guid = sqlite3_column_text(splits_stmt, 1);
		sqlite3_bind_text(acc_stmt, 1, (const char *)account_guid,
				  sqlite3_column_bytes(splits_stmt, 1),
				  SQLITE_STATIC);
		sqlite3_step(acc_stmt);

		len = asprintf(&item, "%.10s %-54s %7.2f", date, desc,
			       amnt / 100.0f);
		if (len == -1) {
			printec("asprintf() failed in %s\n", __func__);
			exit(EXIT_FAILURE);
		}
		account = (const char *)sqlite3_column_text(acc_stmt, 0);
		if (strcmp(account, "BANK") == 0) {
			*income += amnt;
			ac_slist_add(&i_list, item);
		} else if (strcmp(account, "EXPENSE") == 0) {
			*expenses += amnt;
			ac_slist_add(&e_list, item);
		} else {
			printec("Unknown account type : %s\n", account);
			exit(EXIT_FAILURE);
		}

		sqlite3_reset(acc_stmt);
		sqlite3_reset(splits_stmt);
	}

	sqlite3_finalize(acc_stmt);
	sqlite3_finalize(splits_stmt);
	sqlite3_finalize(trans_stmt);
	sqlite3_close(db);

	printc("Items for period #BOLD#%s#RST# to #BOLD#%s#RST#\n\n",
	       start, end);
	printc("#GREEN#  Income(s) :-#RST#\n");
	list_for_each(p, i_list)
		printf("    %s\n", (char *)p->data);
	printc("#CHARC#%79s#RST#", "------------\n");
	printc("#BOLD#%77.2f#RST#\n", *income / 100.0f);
	printf("\n");
	printc("#RED#  Expense(s) :-#RST#\n");
	list_for_each(p, e_list)
		printf("    %s\n", (char *)p->data);
	printc("#CHARC#%79s#RST#", "------------\n");
	printc("#BOLD#%77.2f#RST#\n", *expenses / 100.0f);

	ac_slist_destroy(&i_list, free);
	ac_slist_destroy(&e_list, free);
}

static void print_bread_crumb(const char *bread_crumb[])
{
	char str[192];
	int len = 0;

	if (!*bread_crumb) {
		printc(" #BOLD#/#RST#\n");
		return;
	}

	for ( ; *bread_crumb != NULL; bread_crumb++) {
		snprintf(str + len, sizeof(str) - len, "%s / ", *bread_crumb);
		len = strlen(str);
	}
	str[len - 3] = '\0';
	printc("#BOLD# %s#RST#\n", str);
}

#define MAX_BREAD_CRUMB_LVL		16
static void print_json_tree(json_t *obj, const char *bread_crumb[], int level,
			    bool (*print_json_tree_cb)(const char *key,
						       json_t *value))
{
	const char *key;
	json_t *value;
	bool done_bread_crumb = false;

	json_object_foreach(obj, key, value) {
		char val[64];

		switch (json_typeof(value)) {
		case JSON_OBJECT:
			bread_crumb[level] = key;
			print_json_tree(value, bread_crumb, ++level,
					print_json_tree_cb);
			goto decr_level;
		case JSON_ARRAY: {
			json_t *aobj;
			size_t index;
			size_t size = json_array_size(value);

			bread_crumb[level++] = key;
			json_array_foreach(value, index, aobj) {
				print_json_tree(aobj, bread_crumb, level,
						print_json_tree_cb);
				if (index < size - 1)
					printf("\n");
			}
			goto decr_level;
		}
		case JSON_STRING:
			snprintf(val, sizeof(val), "%s",
				 json_string_value(value));
			break;
		case JSON_INTEGER:
			snprintf(val, sizeof(val), "%lld",
				 json_integer_value(value));
			break;
		case JSON_REAL:
			snprintf(val, sizeof(val), "%.2f",
				 json_real_value(value));
			break;
		case JSON_TRUE:
		case JSON_FALSE:
			snprintf(val, sizeof(val), "%s",
				 json_is_true(value) ? "true" : "false");
			break;
		case JSON_NULL:
			sprintf(val, "null");
			break;
		}

		if (!done_bread_crumb) {
			print_bread_crumb(bread_crumb);
			done_bread_crumb = true;
		}

		if (print_json_tree_cb) {
			bool printed;

			printed = print_json_tree_cb(key, value);
			if (printed)
				continue;
		}
		printc("#CHARC# %*s :#RST# %s\n", JKEY_FW, key, val);
		continue;

decr_level:
		level--;
		bread_crumb[level] = NULL;
		done_bread_crumb = false;
	}
}

static void display_messages(const json_t *msgs_obj, const char *fmt,
			     const char *mtype)
{
	json_t *msgs;
	json_t *msg;
	size_t index;

	msgs = json_object_get(msgs_obj, mtype);
	if (!msgs)
		return;

	printc("\n #CHARC#----#RST# %s #CHARC#----#RST#\n", fmt);

	json_array_foreach(msgs, index, msg) {
		json_t *id;
		json_t *text;

		id = json_object_get(msg, "id");
		text = json_object_get(msg, "text");
		printf(" [\n   %s: %s\n ]\n", json_string_value(id),
		       json_string_value(text));
	}
}

static int display_end_of_year_est(const char *tax_year, const char *cid)
{
	json_t *result;
	json_t *obj;
	char *jbuf __cleanup_free;
	const char *params[2];
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = {};
	int err;

	params[0] = tax_year;
	params[1] = cid;

	err = mtd_ep(MTD_API_EP_ICAL_GET, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get calculation. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	printsc("End of Year estimate for #BOLD#%s#RST#\n", cid);

	result = get_result_json(jbuf);

	JKEY_FW = 32;
	printc("#BOLD# Summary#RST#:-\n");
	obj = json_object_get(result, "calculation");
	obj = json_object_get(obj, "endOfYearEstimate");
	print_json_tree(obj, bread_crumb, 0, NULL);

	json_decref(result);

	return 0;
}

static void display_calculation_messages(const json_t *msgs)
{
	if (!msgs)
		return;

	display_messages(msgs, MSG_ERR, "errors");
	display_messages(msgs, MSG_WARN, "warnings");
	display_messages(msgs, MSG_INFO, "info");
}

static void display_calculation(json_t *obj)
{
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1];
	json_t *tmp;
	const json_t *msgs;

	tmp = json_object_get(obj, "messages");
	msgs = json_copy(tmp);
	json_object_del(obj, "messages");
	json_object_del(obj, "links");

	JKEY_FW = 36;
	memset(bread_crumb, 0, sizeof(char *) * MAX_BREAD_CRUMB_LVL);
	print_json_tree(obj, bread_crumb, 0, NULL);
	display_calculation_messages(msgs);
}

static int get_calculation(const char *tax_year, const char *cid)
{
	json_t *result;
	char *jbuf __cleanup_free;
	const char *params[2];
	int err;
	int fib_sleep = -1;

	params[0] = tax_year;
	params[1] = cid;

again:
	err = mtd_ep(MTD_API_EP_ICAL_GET, NULL, &jbuf, params);
	if ((err && err != MTD_ERR_REQUEST) ||
	    (err == MTD_ERR_REQUEST && fib_sleep == 5)) {
		printec("Couldn't get calculation. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	} else if (err == MTD_ERR_REQUEST) {
		fib_sleep = next_fib(fib_sleep);
		printic("Trying to get calculation again in "
			"#BOLD#%d#RST# second(s)\n", fib_sleep);
		fflush(stdout);
		sleep(fib_sleep);

		goto again;
	}

	result = get_result_json(jbuf);
	printsc("Calculation for #BOLD#%s#RST#\n", tax_year);
	display_calculation(result);
	json_decref(result);

	return 0;
}

static int final_declaration(int argc, char *argv[])
{
	json_t *result;
	json_t *cid_obj;
	char *jbuf __cleanup_free;
	char *s;
	const char *cid;
	const char *params[3];
	char submit[32];
	int ret = -1;
	int err;

	if (argc < 3) {
		disp_usage();
		return -1;
	}

	params[0] = argv[2];	/* taxYear */
	params[1] = "intent-to-finalise";

	err = mtd_ep(MTD_API_EP_ICAL_TRIGGER, NULL, &jbuf, params);
	if (err) {
		printec("Final declartion calculation failed. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	cid_obj = json_object_get(result, "calculationId");
	cid = json_string_value(cid_obj);

	printsc("Final declaration calculationId: #BOLD#%s#RST#\n", cid);

	err = get_calculation(params[0], cid);
	if (err)
		goto out_free_json;

	printf("\n");
	printc(FINAL_DECLARATION);
	printf("\n");
	printcc("Submit 'Final Declaration' for this TAX return? (y/N)> ");

	s = fgets(submit, sizeof(submit), stdin);
	if (!s || (*submit != 'y' && *submit != 'Y'))
		goto out_ok;

	printf("\n");
	printic("About to submit a #TANG#Final Declaration#RST# for "
		"#BOLD#%s#RST#\n\n", params[0]);
	printic("As a final check measure, just enter 'i agree' at the\n");
	printic("prompt. Anything else will abandon the process.\n");
	printf("\n");
	printcc("Enter (without the quotes) 'i agree'> ");

	s = fgets(submit, sizeof(submit), stdin);
	if (!s || strcmp(submit, "i agree\n") != 0)
		goto out_ok;

	free(jbuf);

	params[1] = cid;
	params[2] = "final-declaration";

	err = mtd_ep(MTD_API_EP_ICAL_FINAL_DECLARATION, NULL, &jbuf, params);
	if (err) {
		printec("Failed to submit 'Final Declaration'. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_json;
	}

	printsc("Final Declaration done.\n");

out_ok:
	ret = 0;

out_free_json:
	json_decref(result);

	return ret;
}

static int get_eop_obligations(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *period;
	char qs[192];
	char *jbuf __cleanup_free;
	const char *params[1];
	size_t index;
	int err;

	if (argc > 2 && argc < 4) {
		disp_usage();
		return -1;
	}

	snprintf(qs, sizeof(qs), "?typeOfBusiness=%s&businessId=%s",
		 BUSINESS_TYPE, BUSINESS_ID);
	if (argc > 2) {
		int len = strlen(qs);

		len += snprintf(qs + len, sizeof(qs) - len, "&fromDate=%s",
				argv[2]);
		snprintf(qs + len, sizeof(qs) - len, "&toDate=%s", argv[3]);
	}

	params[0] = qs;

	err = mtd_ep(MTD_API_EP_OB_GET_EPSO, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get End of Period Statement(s). (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	printsc("End of Period Statement Obligations\n");

	result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");
	obs = json_array_get(obs, 0);
	obs = json_object_get(obs, "obligationDetails");

	printc("#CHARC#  %12s %11s %13s %15s %7s#RST#\n",
	       "start", "end", "due", "status", "@" );
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "---------#RST#\n");
	json_array_foreach(obs, index, period) {
		json_t *start_obj = json_object_get(period, "periodStartDate");
		json_t *end_obj = json_object_get(period, "periodEndDate");
		json_t *due_obj = json_object_get(period, "dueDate");
		json_t *recvd_obj = json_object_get(period, "receivedDate");
		json_t *status_obj = json_object_get(period, "status");
		const char *start = json_string_value(start_obj);
		const char *end = json_string_value(end_obj);
		const char *due = json_string_value(due_obj);
		const char *recvd = json_string_value(recvd_obj);
		const char *status = json_string_value(status_obj);
		bool met = *status == 'F' ? true : false;

		printc("%s  %15s %12s %13s %9c%s#HI_GREEN#%15s#RST#\n",
		       get_period_color(start, end, due, met),
		       start, end, due, *status, "#RST#", met ? recvd : "");
        }

	json_decref(result);

	return 0;
}

static const struct {
	const char *exempt_code;
	const char *desc;
} class4_nic_ecode_map[] = {
	{},
	{ "001", "Non Resident" },
	{ "002", "Trustee" },
	{ "003", "Diver" },
	{ "004", "Employed earner taxed under ITTOIA 2005" },
	{ "005", "Over state pension age" },
	{ "006", "Under 16" }
};

static bool print_c4nic_excempt_type(const char *key, json_t *value)
{
	const char *code;

	if (strcmp(key, "exemptionCode") != 0)
		return false;

	code = json_string_value(value);
	printc("#CHARC# %*s :#RST# %s (%s)\n", JKEY_FW, key, code,
	       class4_nic_ecode_map[atoi(code)].desc);

	return true;
}

static int disp_annual_summary(json_t *root)
{
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = {};

	if (!root)
		return -1;

	JKEY_FW = 36;
	print_json_tree(root, bread_crumb, 0, print_c4nic_excempt_type);

	return 0;
}

static int trigger_calculation(const char *tax_year, const char *type)
{
	json_t *result;
	json_t *cid_obj;
	char *jbuf __cleanup_free;
	const char *cid;
	const char *params[2];
	int ret = -1;
	int err;

	params[0] = tax_year;
	params[1] = type;

	err = mtd_ep(MTD_API_EP_ICAL_TRIGGER, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't trigger calculation. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	printsc("Triggered calculation for #BOLD#%s#RST#\n", tax_year);

	result = get_result_json(jbuf);
	cid_obj = json_object_get(result, "id");
	cid = json_string_value(cid_obj);

	err = get_calculation(tax_year, cid);
	if (err) {
		printec("Couldn't get calculation for %s/%s.\n", cid,
			tax_year);
		goto out_free_json;
	}

	ret = 0;

out_free_json:
	json_decref(result);

	return ret;
}

static const char *get_editor(void)
{
	const char *editor = getenv("VISUAL");

	if (!editor)
		editor = getenv("EDITOR");
	if (!editor)
		editor = DEFAULT_EDITOR;

	return editor;
}

extern char **environ;
static int annual_summary(const char *tax_year)
{
	json_t *result;
	char *jbuf __cleanup_free;
	char *s;
	char tpath[PATH_MAX];
	char submit[3] = "\0";
	const char *params[2];
	int tmpfd;
	int ret = -1;
	int err;

	params[0] = BUSINESS_ID;
	params[1] = tax_year;

	err = mtd_ep(MTD_API_EP_SEB_SEAS_GET, NULL, &jbuf, params);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		printec("Couldn't get Annual Summary. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	printsc("Annual Summary for #BOLD#%s#RST#\n", tax_year);

	snprintf(tpath, sizeof(tpath), "/tmp/.itsa_annual_summary.tmp.%d.json",
		 getpid());
	tmpfd = open(tpath, O_CREAT|O_TRUNC|O_RDWR|O_EXCL, 0666);
	if (tmpfd == -1) {
		printec("Couldn't open %s in %s\n", tpath, __func__);
		perror("open");
		return -1;
	}

	result = get_result_json(jbuf);
	if (json_is_null(result))
		result = json_pack("{}");

again:
	err = disp_annual_summary(result);
	if (err)
		goto out_free_json;
	json_dumpfd(result, tmpfd, JSON_INDENT(4));
	lseek(tmpfd, 0, SEEK_SET);
	printf("\n");
	printcc("Submit (s), Edit (e), Quit (Q)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s)
		goto again;

	switch (*submit) {
	case 's':
	case 'S': {
		struct mtd_dsrc_ctx dsctx = {
			.data_src.fd = tmpfd,
			.src_type = MTD_DATA_SRC_FD
		};

		free(jbuf);

		err = mtd_ep(MTD_API_EP_SEB_SEAS_AMEND, &dsctx, &jbuf, params);
		if (err) {
			printec("Couldn't update Annual Summary. (%s)\n%s\n",
				mtd_err2str(err), jbuf);
			goto out_free_json;
		}

		printsc("Updated Annual Summary for #BOLD#%s#RST#\n",
			tax_year);

		err = trigger_calculation(tax_year, "intent-to-finalise");
		if (err)
			goto out_free_json;

		ret = 0;
		break;
	}
	case 'e':
	case 'E': {
		const char *args[3] = {};
		int child_pid;
		int status;

		args[0] = get_editor();
		args[1] = tpath;
		posix_spawnp(&child_pid, args[0], NULL, NULL,
			     (char * const *)args, environ);
		waitpid(child_pid, &status, 0);

		json_decref(result);
		result = json_loadfd(tmpfd, 0, NULL);
		err = ftruncate(tmpfd, 0);
		if (err) {
			printec("ftruncate failed in %s\n", __func__);
			perror("ftruncate");
			goto out_free_json;
		}
		lseek(tmpfd, 0, SEEK_SET);

		goto again;
	}
	default:
		ret = -2;
		break;
	}

out_free_json:
	json_decref(result);

	close(tmpfd);
	unlink(tpath);

	return ret;
}

static int update_annual_summary(int argc, char *argv[])
{
	if (argc < 3) {
		disp_usage();
		return -1;
	}

	return annual_summary(argv[2]);
}

static int set_period(const char *tax_year, const char *start, const char *end,
		      long income, long expenses)
{
	ac_jsonw_t *json;
	char *jbuf __cleanup_free;
	const char *params[2];
	struct mtd_dsrc_ctx dsctx;
	int err;
	int ret = 0;

	json = ac_jsonw_init();

	ac_jsonw_add_object(json, "periodDates");
	ac_jsonw_add_str(json, "periodStartDate", start);
	ac_jsonw_add_str(json, "periodEndDate", end);
	ac_jsonw_end_object(json);

	ac_jsonw_add_object(json, "periodIncome");
	ac_jsonw_add_real(json, "turnover", income / 100.0f, 2);
	ac_jsonw_add_real(json, "other", 0.0, 2);
	ac_jsonw_add_real(json, "taxTakenOffTradingIncome", 0.0, 2);
	ac_jsonw_end_object(json);

	ac_jsonw_add_object(json, "periodExpenses");
	ac_jsonw_add_real(json, "consolidatedExpenses", expenses / 100.0f, 2);
	ac_jsonw_end_object(json);

	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	params[0] = BUSINESS_ID;
	params[1] = tax_year;

	err = mtd_ep(MTD_API_EP_SEB_SECPS_AMEND, &dsctx, &jbuf, params);
	if (err) {
		printec("Failed to set period. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		ret = -1;
	} else {
		printf("\n");
		printsc("Set period for #BOLD#%s#RST# to #BOLD#%s#RST#\n",
			start, end);
	}

	ac_jsonw_free(json);

	return ret;
}

static int view_end_of_year_estimate(void)
{
	json_t *result;
	json_t *obs;
	char *jbuf __cleanup_free;
	const char *cid = NULL;
	const char *params[2];
	char tyear[TAX_YEAR_SZ + 1];
	size_t nr_calcs;
	int err;
	int ret = -1;

	get_tax_year(NULL, tyear);

	params[0] = tyear;
	params[1] = "?calculationType=intent-to-finalise";

	err = mtd_ep(MTD_API_EP_ICAL_LIST, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get calculations list. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "calculations");
	nr_calcs = json_array_size(obs);
	while (nr_calcs--) {
		json_t *calc = json_array_get(obs, nr_calcs);
		json_t *type = json_object_get(calc, "calculationType");
		json_t *cid_obj;

		if (strcmp(json_string_value(type), "inYear") != 0)
			continue;

		cid_obj = json_object_get(calc, "calculationId");
		cid = json_string_value(cid_obj);

		break;
	}

	if (!cid) {
		printec("No inYear calculation found for #BOLD#%s#RST#\n",
			tyear);
		goto out_free_json;
	}

	printsc("Found inYear calculation for #BOLD#%s#RST#\n", tyear);
	display_end_of_year_est(tyear, cid);

	ret = 0;

out_free_json:
	json_decref(result);

	return ret;
}

struct calc_id {
	const char *id;
	const char *tax_year;
};

static void free_calc_id(void *data)
{
	struct calc_id *cid = data;

	if (!cid)
		return;

	free((char *)cid->id);
	free((char *)cid->tax_year);
	free(cid);
}

static int list_calculations(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *calculation;
	char *jbuf __cleanup_free;
	char *s;
	char qs[20] = "\0";
	char submit[4];
	const char *params[2] = {};
	size_t index;
	ac_slist_t *calcs = NULL;
	struct calc_id *cid;
	int err;
	int ret = -1;

	if (argc < 3) {
		disp_usage();
		return ret;
	}

	params[0] = argv[2];	/* taxYear */

	if (argc == 4) {
		snprintf(qs, sizeof(qs), "?calculationType=%s", argv[3]);
		params[1] = qs;
	}

	err = mtd_ep(MTD_API_EP_ICAL_LIST, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get calculations list. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	printsc("Got list of calculations\n");

	result = get_result_json(jbuf);
	obs = json_object_get(result, "calculations");

	printc("#CHARC#  %3s %12s %26s %29s #RST#\n",
	       "idx", "tax_year", "calculation_id", "type");
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "-----------------#RST#\n");
	json_array_foreach(obs, index, calculation) {
		json_t *id_obj = json_object_get(calculation,
						 "calculationId");
		json_t *type = json_object_get(calculation, "calculationType");
		const char *id = json_string_value(id_obj);

		printc("  #BOLD#%2zu#RST#%13s %39s %18s\n",
		       index + 1, params[0], id, json_string_value(type));

		cid = malloc(sizeof(*cid));
		cid->id = strdup(id);
		cid->tax_year = strdup(params[0]);
		ac_slist_add(&calcs, cid);
        }

	printf("\n");
	printcc("Select a calculation to view (n) or quit (Q)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit < '1' || *submit > '9')
		goto out_free_json;

	index = atoi(submit) - 1;
	cid = ac_slist_nth_data(calcs, index);
	get_calculation(cid->tax_year, cid->id);

out_free_json:
	ac_slist_destroy(&calcs, free_calc_id);
	json_decref(result);

	ret = 0;

	return ret;
}

static int __period_update(const char *tax_year, const char *start,
			   const char *end)
{
	long income;
	long expenses;
	int err;
	char *s;
	char submit[3];

	get_data(start, end, &income, &expenses);

	printcc("Submit? (y/N)> ");
	s = fgets(submit, sizeof(submit), stdin);
        if (!s || (*submit != 'y' && *submit != 'Y'))
                return 0;

	err = set_period(tax_year, start, end, income, expenses);
	if (err)
		return -1;

	err = trigger_calculation(tax_year, "in-year");
	if (err)
		return -1;

	return 0;
}

static int update_period(int argc, char *argv[])
{
	int err;
	char start[11];
	char end[11];

	if (argc != 4) {
		disp_usage();
		return -1;
	}

	memcpy(start, argv[3], 10);
	start[10] = '\0';
	memcpy(end, argv[3] + 11, 10);
	end[10] = '\0';

	err = __period_update(argv[2], start, end);
	if (err)
		return -1;

	return 0;
}

static int get_period(char **start, char **end)
{
	json_t *result;
	json_t *obs;
	json_t *period;
	size_t index;
	char qs[128];
	char *jbuf __cleanup_free;
	const char *params[1];
	int err;
	int ret = -1;

	snprintf(qs, sizeof(qs), "?typeOfBusiness=%s&businessId=%s",
		 BUSINESS_TYPE, BUSINESS_ID);
	params[0] = qs;
	err = mtd_ep(MTD_API_EP_OB_GET_IEO, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get list of obligations. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");
	if (!obs)
		goto out_free_json;
	obs = json_array_get(obs, 0);
	obs = json_object_get(obs, "obligationDetails");
	json_array_foreach(obs, index, period) {
		json_t *status;
		json_t *start_obj;
		json_t *end_obj;

		status = json_object_get(period, "status");
		if (strcmp(json_string_value(status), "Fulfilled") == 0)
			continue;

		start_obj = json_object_get(period, "periodStartDate");
		end_obj = json_object_get(period, "periodEndDate");
		*start = strdup(json_string_value(start_obj));
		*end = strdup(json_string_value(end_obj));

		ret = 0;
		break;
        }

out_free_json:
	json_decref(result);

	return ret;
}

static int create_period(int argc, char *argv[])
{
	char *start;
	char *end;
	int ret = 0;
	int err;

	if (argc != 3 && argc != 5) {
		disp_usage();
		return -1;
	}

	if (argc == 5) {
		start = strdup(argv[3]);
		end = strdup(argv[4]);
	} else {
		err = get_period(&start, &end);
		if (err)
			return -1;
	}

	err = __period_update(argv[2], start, end);
	if (err)
		ret = -1;

	free(start);
	free(end);

	return ret;
}

static int list_periods(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *period;
	char qs[192];
	int err;
	size_t index;
	char *jbuf __cleanup_free;
	const char *params[1];

	if (argc > 2 && argc < 4) {
		disp_usage();
		return -1;
	}

	snprintf(qs, sizeof(qs), "?typeOfBusiness=%s&businessId=%s",
		 BUSINESS_TYPE, BUSINESS_ID);
	if (argc > 2) {
		int len = strlen(qs);

		len += snprintf(qs + len, sizeof(qs) - len, "&fromDate=%s",
				argv[2]);
		snprintf(qs + len, sizeof(qs) - len, "&toDate=%s", argv[3]);
	}

	params[0] = qs;
	err = mtd_ep(MTD_API_EP_OB_GET_IEO, NULL, &jbuf, params);
	if (err) {
		printec("Couldn't get list of obligations. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");
	if (!obs)
		goto out_free_json;
	obs = json_array_get(obs, 0);
	obs = json_object_get(obs, "obligationDetails");

	printc("#CHARC#  %14s %18s %11s %12s %8s#RST#\n",
	       "period_id", "start", "end", "due", "met" );
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "---------#RST#\n");
	json_array_foreach(obs, index, period) {
		json_t *start_obj = json_object_get(period, "periodStartDate");
		json_t *end_obj = json_object_get(period, "periodEndDate");
		json_t *due_obj = json_object_get(period, "dueDate");
		json_t *rec_obj = json_object_get(period, "receivedDate");
		const char *start = json_string_value(start_obj);
		const char *end = json_string_value(end_obj);
		const char *due = json_string_value(due_obj);

		printc("%s  %s_%-14s %-12s %-12s %-12s%s %s\n",
		       get_period_color(start, end, due,
					rec_obj ? true : false),
		       start, end, start, end, due,
		       "#RST#", rec_obj ? STRUE : SFALSE);
        }

out_free_json:
	json_decref(result);

	return 0;
}

#define SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS	"A-Za-z0-9 &'()*,-./@£"
#define SAVINGS_ACCOUNT_NAME_REGEX \
	"^[" SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS "]{1,32}$"
static int add_savings_account(void)
{
	char *jbuf __cleanup_free;
	char *s;
	char submit[33]; /* Max allowed account name is 32 chars (+ nul) */
	ac_jsonw_t *json;
	struct mtd_dsrc_ctx dsctx;
	regex_t re;
	regmatch_t pmatch[1];
	int ret;
	int err;

	err = regcomp(&re, SAVINGS_ACCOUNT_NAME_REGEX, REG_EXTENDED);
	if (err) {
		perror("regcomp");
		return -1;
	}

	printic("Enter a friendly account name, allowed characters are :-\n"
		"\n\t#BOLD#" SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS "#RST#\n");

again:
	printf("\n");
	printcc("Name> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit == '\n')
		return 0;

	ac_str_chomp(submit);
	ret = regexec(&re, submit, 1, pmatch, 0);
	if (ret != 0) {
		printec("Invalid name\n");
		goto again;
	}

	json = ac_jsonw_init();
	ac_jsonw_add_str(json, "accountName", submit);
	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	ret = -1;
	err = mtd_ep(MTD_API_EP_ISI_SI_UK_ADD, &dsctx, &jbuf, NULL);
	if (err) {
		printec("Couldn't add savings account. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printsc("Added savings account : #BOLD#%s#RST#\n", submit);

	ret = 0;

out_free:
	regfree(&re);
	ac_jsonw_free(json);

	return ret;
}

static int view_savings_accounts(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *account;
	char tyear[TAX_YEAR_SZ + 1];
	char *jbuf __cleanup_free;
	const char *params[2] = {};
	size_t index;
	int err;
	int ret = -1;

	err = mtd_ep(MTD_API_EP_ISI_SI_UK_LIST, NULL, &jbuf, params);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		printec("Couldn't get list of savings accounts. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		return -1;
	}

	printf("\n%s\n", jbuf);

	if (argc < 3)
		get_tax_year(NULL, tyear);
	else
		snprintf(tyear, sizeof(tyear), "%s", argv[2]);

	printsc("UK Savings Accounts for #BOLD#%s#RST#\n", tyear);

	printc("\n#CHARC#  %8s %26s#RST#\n", "id", "name");
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "#RST#\n");

	result = get_result_json(jbuf);
	obs = json_object_get(result, "savingsAccounts");
	free(jbuf);
	jbuf = NULL; /* avoid potential double free if no accounts */
	json_array_foreach(obs, index, account) {
		json_t *res;
		json_t *said = json_object_get(account, "savingsAccountId");
		json_t *name = json_object_get(account, "accountName");
		json_t *taxed_amnt;
		json_t *untaxed_amnt;
		float taxed_int = -1.0;
		float untaxed_int = -1.0;

		params[0] = tyear;
		params[1] = json_string_value(said);
		err = mtd_ep(MTD_API_EP_ISI_SI_UK_GET_AS, NULL, &jbuf, params);
		if (err) {
			printec("Couldn't retrieve account details. "
				"(%s)\n%s\n", mtd_err2str(err), jbuf);
			goto out_free_json;
		}
		res = get_result_json(jbuf);
		taxed_amnt = json_object_get(res, "taxedUkInterest");
		untaxed_amnt = json_object_get(res, "untaxedUkInterest");

		if (taxed_amnt)
			taxed_int = json_real_value(taxed_amnt);
		if (untaxed_amnt)
			untaxed_int = json_real_value(untaxed_amnt);

		printf("  %-25s %-34s\n",
		       json_string_value(said),
					json_string_value(name) ?
					json_string_value(name) : "N/A");
		if (taxed_int >= 0.0f)
			printc("#CHARC#%25s#RST##BOLD#%12.2f#RST#\n",
			       "taxedUkInterest : ", taxed_int);
		if (untaxed_int >= 0.0f)
			printc("#CHARC#%25s#RST##BOLD#%12.2f#RST#\n",
			       "untaxedUkInterest : ", untaxed_int);
		printf("\n");

		json_decref(res);
		free(jbuf), jbuf = NULL;
        }

	ret = 0;

out_free_json:
	json_decref(result);

	return ret;
}

static int get_savings_accounts_list(ac_slist_t **accounts)
{
	json_t *result;
	json_t *obs;
	json_t *account;
	char *jbuf __cleanup_free;
	size_t index;
	int err;

	err = mtd_ep(MTD_API_EP_ISI_SI_UK_LIST, NULL, &jbuf,
		     (const char *[1]){});
	if (err) {
		printec("Couldn't get list of savings accounts. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "savingsAccounts");

	printc("#CHARC#  idx %9s %26s#RST#\n", "id", "name");
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "---#RST#\n");
	json_array_foreach(obs, index, account) {
		json_t *said = json_object_get(account, "savingsAccountId");
		json_t *name = json_object_get(account, "accountName");

		printf("  %2zu    %-22s %s\n", index + 1,
		       json_string_value(said), json_string_value(name));

		ac_slist_add(accounts, strdup(json_string_value(said)));
        }

	json_decref(result);

	return 0;
}

static int amend_savings_account(int argc, char *argv[])
{
	struct mtd_dsrc_ctx dsctx;
	ac_slist_t *accounts = NULL;
	json_t *result;
	json_t *taxed_int;
	json_t *untaxed_int;
	json_t *amnt = json_real(0.0f);
	char *jbuf __cleanup_free;
	char *s;
	char submit[3];
	char tpath[PATH_MAX];
	const char *args[3] = {};
	const char *params[2];
	int child_pid;
	int status;
	int tmpfd;
	int ret = -1;
	int err;

	if (argc < 3) {
		disp_usage();
		return -1;
	}

	get_savings_accounts_list(&accounts);
	printf("\n");
	printcc("Select account to edit (n) or quit (Q)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit < '1' || *submit > '9')
		goto out_free_list;

	params[0] = argv[2];	/* taxYear */
	params[1] = ac_slist_nth_data(accounts, atoi(submit) - 1); /* said */
	if (!params[1]) {
		printec("No such account index\n");
		goto out_free_list;
	}

	err = mtd_ep(MTD_API_EP_ISI_SI_UK_GET_AS, NULL, &jbuf, params);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		printec("Couldn't retrieve account details. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_list;
	}
	if (mtd_http_status_code(jbuf) == MTD_HTTP_NOT_FOUND) {
		printec("No such Savings Account\n");
		goto out_free_list;
	}

	result = get_result_json(jbuf);
	if (!result)
		result = json_pack("{}");
	taxed_int = json_object_get(result, "taxedUkInterest");
	if (!taxed_int)
		json_object_set(result, "taxedUkInterest", amnt);
	untaxed_int = json_object_get(result, "untaxedUkInterest");
	if (!untaxed_int)
		json_object_set(result, "untaxedUkInterest", amnt);

	snprintf(tpath, sizeof(tpath),
		 "/tmp/.itsa_savings_account.tmp.%d.json", getpid());
	tmpfd = open(tpath, O_CREAT|O_TRUNC|O_RDWR|O_EXCL, 0666);
	if (tmpfd == -1) {
		printec("Couldn't open %s in %s\n", tpath, __func__);
		perror("open");
		goto out_free_list;
	}

	json_dumpfd(result, tmpfd, JSON_INDENT(4));
	lseek(tmpfd, 0, SEEK_SET);

	args[0] = get_editor();
	args[1] = tpath;
	posix_spawnp(&child_pid, args[0], NULL, NULL, (char * const *)args,
		     environ);
	waitpid(child_pid, &status, 0);

	dsctx.data_src.fd = tmpfd;
	dsctx.src_type = MTD_DATA_SRC_FD;

	free(jbuf);
	err = mtd_ep(MTD_API_EP_ISI_SI_UK_UPDATE_AS, &dsctx, &jbuf, params);
	if (err) {
		printec("Couldn't update Savings Account. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_close_tmpfd;
	}

	printsc("Updated Savings Account #BOLD#%s#RST#\n", params[1]);
	args[2] = params[0];
	view_savings_accounts(3, (char **)args);

	ret = 0;

out_close_tmpfd:
	close(tmpfd);
	unlink(tpath);

	json_decref(result);

out_free_list:
	ac_slist_destroy(&accounts, free);

	return ret;
}

static int switch_business(void)
{
	json_t *lob;
	json_t *bus;
	json_t *config;
	json_t *bidx;
	json_error_t error;
	size_t idx;
	size_t didx;
	char path[PATH_MAX];
	char *s;
	char submit[6];
	int def_bus = 0;

	snprintf(path, sizeof(path), "%s/" ITSA_CFG, getenv("HOME"));
	config = json_load_file(path, 0, &error);
	if (!config) {
		printec("Couldn't open %s: %s\n", path, error.text);
		return -1;
	}

	bidx = json_object_get(config, "business_idx");
	didx = json_integer_value(bidx);

	lob = json_object_get(config, "businesses");
	printf("\n");
	printc("#CHARC#  cur   %-7s %7s %20s %15s#RST#\n",
	       "idx", "type", "bid", "name");
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "------------------#RST#\n");
	json_array_foreach(lob, idx, bus) {
		json_t *type_obj = json_object_get(bus, "type");
		json_t *bid_obj = json_object_get(bus, "bid");
		json_t *name_obj = json_object_get(bus, "name");
		const char *type = json_string_value(type_obj);
		const char *bid = json_string_value(bid_obj);
		const char *name = json_string_value(name_obj);

		printc("  #BOLD#%2s#RST#    %2zu     %-20s %-19s %s\n",
		       idx == didx ? "*" : "", idx, type, bid, name);
	}
	printf("\n");

again:
	printcc("Select a business to use as default (n)> ");
	s = fgets(submit, sizeof(submit), stdin);
	def_bus = atoi(submit);
	if (!s || *s < '0' || *s > '9' ||
	    def_bus > (int)json_array_size(lob))
		goto again;

	bus = json_array_get(lob, def_bus);
	printf("\n");
	printsc("Using #BOLD#%s#RST# / #BOLD#%s#RST# as default business\n",
		json_string_value(json_object_get(bus, "name")),
		json_string_value(json_object_get(bus, "bid")));

	json_object_set_new(config, "business_idx", json_integer(def_bus));
	json_dump_file(config, path, JSON_INDENT(4));
	json_decref(config);

	return 0;
}

static int set_business(void)
{
	json_t *result = NULL;
	json_t *lob;
	json_t *bus;
	json_t *config;
	json_error_t error;
	json_t *ba;
	size_t idx;
	char *jbuf __cleanup_free;
	char path[PATH_MAX];
	char *s;
	char submit[PATH_MAX];
	int def_bus = 0;
	int err;

	printf("\nLooking up business(es)...\n");
	err = mtd_ep(MTD_API_EP_BD_LIST, NULL, &jbuf, NULL);
	if (err) {
		printec("set_business: Couldn't get list of employments. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	lob = json_object_get(result, "listOfBusinesses");
	if (json_array_size(lob) == 0) {
		printec("set_business: No business(es) found.\n");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/" ITSA_CFG, getenv("HOME"));
	config = json_load_file(path, 0, &error);
	if (!config) {
		printec("set_business: Couldn't open %s: %s\n",
			path, error.text);
		return -1;
	}

	ba = json_array();
	printf("\n");
	printc("#CHARC#  %-7s %7s %20s %15s#RST#\n", "idx", "type", "bid",
	       "name");
	printc("#CHARC#"
	       " ------------------------------------------------------------"
	       "------------------#RST#\n");
	json_array_foreach(lob, idx, bus) {
		json_t *new;
		json_t *type_obj = json_object_get(bus, "typeOfBusiness");
		json_t *bid_obj = json_object_get(bus, "businessId");
		json_t *name_obj = json_object_get(bus, "tradingName");
		const char *type = json_string_value(type_obj);
		const char *bid = json_string_value(bid_obj);
		const char *name = json_string_value(name_obj);

		printf("  %2zu     %-20s %-19s %s\n", idx, type, bid, name);

		new = json_pack("{ s:s, s:s, s:s? }", "type", type, "bid", bid,
				"name", name);
		json_array_append_new(ba, new);
	}
	json_object_set(config, "businesses", ba);

	printf("\n");
	if (json_array_size(lob) > 1) {
again:
		printcc("Select a business to use as default (n)> ");
		s = fgets(submit, sizeof(submit), stdin);
		def_bus = atoi(submit);
		if (!s || *s < '0' || *s > '9' ||
		    def_bus > (int)json_array_size(lob))
			goto again;
	}
	json_object_set_new(config, "business_idx", json_integer(def_bus));

	printf("\n");
	printcc("Enter the data source path for the default business> ");
	s = fgets(submit, sizeof(submit), stdin);
	ac_str_chomp(s);
	bus = json_array_get(ba, def_bus);
	json_object_set_new(bus, "gnc_sqlite", json_string(s));

	json_dump_file(config, path, JSON_INDENT(4));
	printf("\n");
	printic("Set data source path to : #BOLD#%s#RST#\n", s);
	json_decref(ba);
	json_decref(config);

	json_decref(result);

	return 0;
}

static int init_auth(void)
{
	int err;

	err = mtd_init_auth(MTD_API_SCOPE_ITSA, MTD_SCOPE_RD_SA|MTD_SCOPE_WR_SA);
	if (err)
		printec("mtd_init_auth: %s\n", mtd_err2str(err));

	return err;
}

static int do_init_all(const struct mtd_cfg *cfg)
{
	char path[PATH_MAX];
	int dfd;
	int err;

	/* Quick check to see if we alresdy have a libmtdac config... */
	snprintf(path, sizeof(path), "%s/libmtdac/%s", cfg->config_dir,
		 is_prod_api ? "prod-api" : "test-api");
	dfd = open(path, O_PATH|O_DIRECTORY);
	if (dfd != -1) {
		struct stat sb;

		err = fstatat(dfd, "creds.json", &sb, 0);
		if (!err) {
			char *s;
			char submit[3];

			printwc("Existing libmtdac config found @ %s\n", path);
			printcc("Continue? (y/N)> ");
			s = fgets(submit, sizeof(submit), stdin);
			if (!s || (*submit != 'y' && *submit != 'Y'))
				return 0;
			printf("\n");
		}
	}

	printf("Initialising...\n\n");
	err = mtd_init_creds(MTD_API_SCOPE_ITSA);
	if (err) {
		printec("mtd_init_creds: %s\n", mtd_err2str(err));
		return err;
	}

	printf("\n");
	err = mtd_init_nino();
	if (err) {
		printec("mtd_init_nino: %s\n", mtd_err2str(err));
		return err;
	}

	printf("\n");
	err = init_auth();
	if (err)
		return err;

	err = set_business();
	if (err)
		return err;

	printf("\n");
	printsc("Initialisation complete. Re-run command if something looks "
		"wrong.\n");

	return 0;
}

static void print_api_info(void)
{
	struct timespec tp;
	struct tm tm;
	char buf[32] = "\0";

	printic("***\n");
	printic("*** Using %s API\n",
		is_prod_api ? "#RED#PRODUCTION#RST#" : "#TANG#TEST#RST#");
	printic("***\n");
	if (!BUSINESS_ID)
		goto out;
	printic("*** Using business : #BOLD#%s#RST# [#BOLD#%s#RST#]\n",
		BUSINESS_NAME, BUSINESS_ID);
	printic("***\n");

	clock_gettime(CLOCK_REALTIME, &tp);
	localtime_r(&tp.tv_sec, &tm);
	strftime(buf, sizeof(buf), "%FT%T", &tm);
	printic("*** Started @ #BOLD#%s#RST#\n", buf);
	printic("***\n");

out:
	printf("\n");
}

static int read_config(void)
{
	json_t *root;
	json_t *prod_api;
	json_t *bidx_obj;
	json_t *jobj;
	json_t *bus_obj;
	json_t *lob;
	char path[PATH_MAX];
	int ret = -1;

	snprintf(path, sizeof(path), "%s/" ITSA_CFG, getenv("HOME"));

	root = json_load_file(path, 0, NULL);
	if (!root) {
		printec("read_config: Unable to open config : %s\n", path);
		return -1;
	}

	prod_api = json_object_get(root, "production_api");
	is_prod_api = json_is_true(prod_api);

	bidx_obj = json_object_get(root, "business_idx");
	if (!bidx_obj) {
		printec("read_config: No 'business_idx' found.\n");
		goto out_free;
	}
	lob = json_object_get(root, "businesses");
	if (!lob) {
		printec("read_config: No 'businesses' found.\n");
		goto out_free;
	}
	bus_obj = json_array_get(lob, json_integer_value(bidx_obj));
	jobj = json_object_get(bus_obj, "bid");
	itsa_config.bid = strdup(json_string_value(jobj));
	jobj = json_object_get(bus_obj, "type");
	itsa_config.btype = strdup(json_string_value(jobj));
	jobj = json_object_get(bus_obj, "name");
	itsa_config.bname = jobj ? strdup(json_string_value(jobj)) : NULL;
	jobj = json_object_get(bus_obj, "gnc_sqlite");
	itsa_config.gnc = strdup(json_string_value(jobj));

	ret = 0;

out_free:
	json_decref(root);

	return ret;
}

static char *set_prod_name(void *user_data __unused)
{
	return strdup(PROD_NAME);
}

static char *set_ver_cli(void *user_data __unused)
{
	char *buf;
	char *encname;
	char *encver;
	int len;

	encname = mtd_percent_encode(PROD_NAME, -1);
	encver = mtd_percent_encode(GIT_VERSION, -1);

	len = asprintf(&buf, "%s=%s", encname, encver);
	if (len == -1) {
		perror("set_ver_cli/asprintf");
		buf = NULL;
	}

	free(encname);
	free(encver);

	return buf;
}

static const char *get_conf_dir(char *path)
{
	const char *home_dir = getenv("HOME");
	struct stat sb;
	int dfd;
	int err;

	snprintf(path, PATH_MAX, "%s/.config/itsa", home_dir);
	dfd = open(home_dir, O_PATH|O_DIRECTORY);
	if (dfd == -1) {
		printec("get_conf_dir/open: Can't open %s\n", home_dir);
		exit(EXIT_FAILURE);
	}

	err = fstatat(dfd, ".config", &sb, 0);
	if (err)
		mkdirat(dfd, ".config", 0777);
	err = fstatat(dfd, ".config/itsa", &sb, 0);
	if (err)
		mkdirat(dfd, ".config/itsa", 0700);

	close(dfd);

	return path;
}

#define IS_CMD(cmd)		(strcmp(cmd, argv[1]) == 0)
static int dispatcher(int argc, char *argv[], const struct mtd_cfg *cfg)
{
	if (IS_CMD("init"))
		return do_init_all(cfg);
	if (IS_CMD("re-auth"))
		return init_auth();
	if (IS_CMD("switch_business"))
		return switch_business();
	if (IS_CMD("list-periods"))
		return list_periods(argc, argv);
	if (IS_CMD("create-period"))
		return create_period(argc, argv);
	if (IS_CMD("update-period"))
		return update_period(argc, argv);
	if (IS_CMD("update-annual-summary"))
		return update_annual_summary(argc, argv);
	if (IS_CMD("get-end-of-period-statement-obligations"))
		return get_eop_obligations(argc, argv);
	if (IS_CMD("submit-final-declaration"))
		return final_declaration(argc, argv);
	if (IS_CMD("list-calculations"))
		return list_calculations(argc, argv);
	if (IS_CMD("view-end-of-year-estimate"))
		return view_end_of_year_estimate();
	if (IS_CMD("add-savings-account"))
		return add_savings_account();
	if (IS_CMD("view-savings-accounts"))
		return view_savings_accounts(argc, argv);
	if (IS_CMD("amend-savings-account"))
		return amend_savings_account(argc, argv);

	disp_usage();

	return -1;
}

int main(int argc, char *argv[])
{
	int err;
	int ret = EXIT_SUCCESS;
	unsigned int flags = MTD_OPT_GLOBAL_INIT;
	char config_dir[PATH_MAX];
	const char *log_level = getenv("ITSA_LOG_LEVEL");
	const struct mtd_fph_ops fph_ops = {
		.fph_version_cli = set_ver_cli,
		.fph_prod_name = set_prod_name
	};
	const struct mtd_cfg cfg = {
		.fph_ops = &fph_ops,
		.extra_hdrs = extra_hdrs,
		.config_dir = get_conf_dir(config_dir)
	};

	if (argc < 2) {
		disp_usage();
		exit(EXIT_FAILURE);
	}

	extra_hdrs[0] = getenv("ITSA_GOV_TEST_SCENARIO");

	set_colors();

	if (!IS_CMD("init")) {
		err = read_config();
		if (err)
			exit(EXIT_FAILURE);
	}

	print_api_info();

	if (log_level && *log_level == 'd')
		flags |= MTD_OPT_LOG_DEBUG;
	else if (log_level && *log_level == 'i')
		flags |= MTD_OPT_LOG_INFO;

	flags |= MTD_OPT_ACT_OTHER_DIRECT;
	err = mtd_init(flags, &cfg);
	if (err) {
		printec("mtd_init: %s\n", mtd_err2str(err));
		exit(EXIT_FAILURE);
	}

	err = dispatcher(argc, argv, &cfg);
	if (err)
		ret = EXIT_FAILURE;

	mtd_deinit();
	free_config();

	exit(ret);
}
