/* SPDX-License-Identifier: GPL-2.0 */

/*
 * itsa.c - Provide Income TAX Self-Assessment via UK's HMRC MTD API
 *
 * Copyright (c) 2021		Andrew Clayton <andrew@digital-domain.net>
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <regex.h>
#include <linux/limits.h>

#include <sqlite3.h>

#include <jansson.h>

#include <libmtdac/mtd.h>
#include <libmtdac/mtd-sa.h>
#include <libmtdac/mtd-ic.h>
#include <libmtdac/mtd-biss.h>

#include <libac.h>

#define PROD_NAME		"itsa"

#define ITSA_CFG		".config/itsa/config.json"
#define DEFAULT_EDITOR		"vi"

#define list_for_each(list)	for ( ; list; list = list->next)

#define TC_HI_YELLOW		"\e[38;5;11m"
#define TC_HI_GREEN		"\e[38;5;10m"
#define TC_HI_RED		"\e[38;5;9m"
#define TC_GREEN		"\e[38;5;40m"
#define	TC_RED			"\e[38;5;160m"
#define TC_AQUA			"\e[38;5;51m"
#define TC_BLUE			"\e[38;5;75m"
#define TC_CHARC		"\e[38;5;8m"
#define TC_TANG			"\e[38;5;220m"
#define TC_BOLD			"\e[1m"
#define TC_RST			"\e[0m"

#define MSG_INFO		"\e[38;5;33mINFO" TC_RST
#define MSG_WARN		TC_HI_YELLOW "WARNINGS" TC_RST
#define MSG_ERR			TC_HI_RED "ERRORS" TC_RST

#define INFO			"[" TC_BLUE "INFO" TC_RST "] "
#define CONFIRM			"[" TC_CHARC "CONFIRMATION" TC_RST "] "
#define WARNING			"[" TC_HI_YELLOW "WARNING" TC_RST "] "
#define SUCCESS			"[" TC_HI_GREEN "OK" TC_RST "] "
#define ERROR			"[" TC_HI_RED "ERROR" TC_RST "] "

#define STRUE			TC_HI_GREEN "t" TC_RST
#define SFALSE			TC_HI_RED "f" TC_RST

#define TAX_YEAR_SZ		7

#define CRYSTALLISATION_DECLARATION \
INFO "Before you can submit the information displayed here in response\n"\
INFO "to your notice to file from HM Revenue & Customs, you must read\n"\
INFO "and agree to the following statement by selecting (y).\n"\
"\n"\
INFO "The information I have provided is correct and complete to the\n"\
INFO "best of my knowledge and belief. If you give false information\n"\
INFO "you may have to pay financial penalties and face prosecution.\n"\
"\n"\
INFO "By saying yes (y) below, you are declaring that you agree with\n"\
INFO "the above and wish to proceed with final crystallisation.\n"

#define EOP_DECLARATION \
INFO "I confirm that I have reviewed the information provided to establish\n"\
INFO "the taxable profits for the relevant period ending in %s together\n"\
INFO "with the designatory data provided for that period and that it is\n"\
INFO "correct and complete to the best of my knowledge. I understand that I\n"\
INFO "may have to pay financial penalties or face prosecution if I give false"\
"\n"\
INFO "information.\n"

enum period_action {
	PERIOD_CREATE,
	PERIOD_UPDATE,
};

struct api_error {
	const int error;
	const char *str;
};

enum ic_end_of_year_est_error {
	/* 0 success, (-)1 non-specific handled error */
	RULE_CALCULATION_ERROR_MESSAGES_EXIST = 2,
	MATCHING_RESOURCE_NOT_FOUND,
	END_OF_YEAR_ESTIMATE_NOT_PRESENT,
};

static const struct api_error ic_end_of_year_est_errors[] = {
	{
		RULE_CALCULATION_ERROR_MESSAGES_EXIST,
		"RULE_CALCULATION_ERROR_MESSAGES_EXIST"
	}, {
		MATCHING_RESOURCE_NOT_FOUND,
		"MATCHING_RESOURCE_NOT_FOUND"
	}, {
		END_OF_YEAR_ESTIMATE_NOT_PRESENT,
		"END_OF_YEAR_ESTIMATE_NOT_PRESENT"
	},

	{ }
};

static struct {
	const char *gnc;
	const char *seid;
	const char *nino;
	const char *client_id;
	const char *client_secret;
} itsa_config;
#define SEID	itsa_config.seid

static char const *extra_hdrs[5];

static bool is_prod_api;

static int JKEY_FW;

static void disp_usage(void)
{
	printf("Usage: itsa COMMAND [OPTIONS]\n\n");
	printf("Commands\n");
	printf("    init\n");
	printf("    re-auth\n");
	printf("    list-periods [<start> <end>]\n");
	printf("    create-period\n");
	printf("    update-period <period_id>\n");
	printf("    update-annual-summary <tax_year>\n");
	printf("    get-end-of-period-statement-obligations\n");
	printf("    submit-end-of-period-statement <start> <end>\n");
	printf("    crystallise <tax_year>\n");
	printf("    list-calculations [tax_year]\n");
	printf("    view-end-of-year-estimate\n");
	printf("    add-savings-account\n");
	printf("    view-savings-accounts\n");
	printf("    amend-savings-account\n");
}

static void free_config(void)
{
	free((void *)itsa_config.gnc);
	free((void *)itsa_config.seid);
	free((void *)itsa_config.nino);
	free((void *)itsa_config.client_id);
	free((void *)itsa_config.client_secret);
}

/*
 * Simple wrapper around time(2) that allows to override the
 * current date.
 */
static time_t xtime(void)
{
	const char *set_date = getenv("ITSA_SET_DATE");
	struct tm tm = { 0 };

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

static int get_api_err(const struct api_error *errors, const char *buf)
{
	json_t *result = get_result_json(buf);
	json_t *code = json_object_get(result, "code");
	int ret = -1;

	if (!code)
		return -1;

	while (errors->str) {
		if (strcmp(json_string_value(code), errors->str) == 0) {
			ret = -errors->error;
			break;
		}

		errors++;
	}
	json_decref(result);

	return ret;
}

/*
 * For doing request back-off, following the Fibonaci Sequence
 * (skipping 0)
 */
static int next_fib(int last, int *state)
{
	int Fn;

	if (last == -1)
		last = *state = 0;
	else if (last == 0)
		return 1;
	else
		Fn = *state + last;

	*state = last;
	if (*state == 0)
		return 1;

	return Fn;
}

static const char *get_period_color(const char *start, const char *end,
				    const char *due, bool met)
{
	time_t now = xtime();
	time_t st;
	time_t et;
	time_t dt;
	struct tm stm = { 0 };
	struct tm etm = { 0 };
	struct tm dtm = { 0 };

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
		return TC_GREEN;
	else if (now > et && now <= dt)
		return TC_TANG;
	else if (now >= st && now <= et)
		return "";
	else if (!met && now > dt)
		return TC_RED;

	return TC_CHARC;
}

static char *get_tax_year(const char *date, char *buf)
{
	struct tm tm = { 0 };
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
	sqlite3 *db;
	char sql[512];
	ac_slist_t *i_list = NULL;
	ac_slist_t *e_list = NULL;
	ac_slist_t *p;

	*income = *expenses = 0;

	sqlite3_open(itsa_config.gnc, &db);
	snprintf(sql, sizeof(sql),
		 "SELECT * FROM transactions WHERE "
		 "post_date >= '%s' AND post_date <= '%s'", start, end);
	sqlite3_prepare_v2(db, sql, -1, &trans_stmt, NULL);

	while (sqlite3_step(trans_stmt) == SQLITE_ROW) {
		sqlite3_stmt *splits_stmt;
		sqlite3_stmt *acc_stmt;
		char *item;
		const char *account;
		const unsigned char *date = sqlite3_column_text(trans_stmt, 3);
		const unsigned char *desc = sqlite3_column_text(trans_stmt, 5);
		const unsigned char *tx_guid =
			sqlite3_column_text(trans_stmt, 0);
		const unsigned char *account_guid;
		long amnt;
		int len;

		snprintf(sql, sizeof(sql),
			 "SELECT value_num, account_guid FROM splits WHERE "
			 "tx_guid = '%s' AND value_num > 0 LIMIT 1", tx_guid);
		sqlite3_prepare_v2(db, sql, -1, &splits_stmt, NULL);
		sqlite3_step(splits_stmt);

		amnt = sqlite3_column_int(splits_stmt, 0);
		account_guid = sqlite3_column_text(splits_stmt, 1);
		snprintf(sql, sizeof(sql),
			 "SELECT account_type FROM accounts WHERE guid = '%s'",
			 account_guid);
		sqlite3_prepare_v2(db, sql, -1, &acc_stmt, NULL);
		sqlite3_step(acc_stmt);

		len = asprintf(&item, "%.10s %-54s %7.2f", date, desc,
			       amnt / 100.0f);
		if (len == -1) {
			fprintf(stderr,
				ERROR "asprintf() failed in %s\n", __func__);
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
			fprintf(stderr,
				ERROR "Unknown account type : %s\n", account);
			exit(EXIT_FAILURE);
		}

		sqlite3_finalize(splits_stmt);
		sqlite3_finalize(acc_stmt);
	}
	sqlite3_finalize(trans_stmt);
	sqlite3_close(db);

	printf("Items for period " TC_BOLD "%s" TC_RST " to " TC_BOLD "%s"
	       TC_RST "\n\n", start, end);
	printf(TC_GREEN "  Income(s) :-" TC_RST "\n");
	p = i_list;
	list_for_each(p)
		printf("    %s\n", (char *)p->data);
	printf(TC_CHARC "%79s" TC_RST, "------------\n");
	printf(TC_BOLD "%77.2f" TC_RST "\n", *income / 100.0f);
	printf("\n");
	printf(TC_RED "  Expense(s) :-" TC_RST "\n");
	p = e_list;
	list_for_each(p)
		printf("    %s\n", (char *)p->data);
	printf(TC_CHARC "%79s" TC_RST, "------------\n");
	printf(TC_BOLD "%77.2f" TC_RST "\n", *expenses / 100.0f);

	ac_slist_destroy(&i_list, free);
	ac_slist_destroy(&e_list, free);
}

static void display_messages(const json_t *result, const char *fmt,
			     const char *mtype)
{
	json_t *msgs;
	json_t *msg;
	size_t index;

	msgs = json_object_get(result, mtype);
	if (!msgs)
		return;

	printf("\n "
	       TC_CHARC "---- " TC_RST "%s"  TC_CHARC " ----" TC_RST "\n",
	       fmt);

	json_array_foreach(msgs, index, msg) {
		json_t *text;

		text = json_object_get(msg, "text");
		printf(" [\n   %s\n ]\n", json_string_value(text));
	}
}

static int display_calculation_messages(const char *cid)
{
	json_t *result;
	char *jbuf;
	int err;
	int ret = -1;

	err = mtd_ic_sa_get_messages(cid, NULL, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get calculation messages. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	result = get_result_json(jbuf);

	display_messages(result, MSG_ERR, "errors");
	display_messages(result, MSG_WARN, "warnings");
	display_messages(result, MSG_INFO, "info");

	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

static void print_bread_crumb(const char *bread_crumb[])
{
	char str[128];
	int len = 0;

	if (!*bread_crumb) {
		printf(" " TC_BOLD "/" TC_RST "\n");
		return;
	}

	for ( ; *bread_crumb != NULL; bread_crumb++)
		len += snprintf(str + len, sizeof(str) - len, "%s / ",
				*bread_crumb);
	str[len - 3] = '\0';
	printf(TC_BOLD " %s" TC_RST "\n", str);
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
		char val[128];

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
		printf(TC_CHARC " %*s :" TC_RST " %s\n", JKEY_FW, key, val);
		continue;

decr_level:
		level--;
		bread_crumb[level] = NULL;
		done_bread_crumb = false;
	}
}

static int display_end_of_year_est(const char *cid)
{
	json_t *result;
	json_t *obj;
	char *jbuf;
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = { NULL };
	int ret = -1;
	int err;

	err = mtd_ic_sa_get_end_of_year_est(cid, &jbuf);
	if (err) {
		enum ic_end_of_year_est_error ecode;

		fprintf(stderr, ERROR "Couldn't get End of Year estimate. ");

		ecode = get_api_err(ic_end_of_year_est_errors, jbuf);
		switch (-ecode) {
		case RULE_CALCULATION_ERROR_MESSAGES_EXIST:
			fprintf(stderr, "Error messages exist.\n");
			break;
		case MATCHING_RESOURCE_NOT_FOUND:
		case END_OF_YEAR_ESTIMATE_NOT_PRESENT:
			fprintf(stderr, "No end of Year Estimates.\n");
			break;
		default:
			fprintf(stderr, "(%s)\n%s\n", mtd_err2str(err), jbuf);
		}

		ret = ecode;
		goto out_free_buf;
	}

	printf(SUCCESS
	       "End of Year estimate for " TC_BOLD "%s" TC_RST "\n", cid);

	result = get_result_json(jbuf);

	printf(TC_BOLD " Summary" TC_RST ":-\n");
	obj = json_object_get(result, "summary");
	print_json_tree(obj, bread_crumb, 0, NULL);

	memset(bread_crumb, 0, sizeof(char *) * MAX_BREAD_CRUMB_LVL);
	printf(TC_BOLD " Details" TC_RST ":-\n");
	obj = json_object_get(result, "detail");
	print_json_tree(obj, bread_crumb, 0, NULL);

	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

static int display_calulated_a_d_r(const char *cid)
{
	json_t *result;
	json_t *obj;
	char *jbuf;
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = { NULL };
	int ret = -1;
	int err;

	err = mtd_ic_sa_get_allowances_deductions_reliefs(cid, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get "
			"allowances, deductions & reliefs calulation. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	printf(SUCCESS
	       "Allowances, deductions & reliefs calculation for " TC_BOLD
	       "%s" TC_RST "\n", cid);

	result = get_result_json(jbuf);

	printf(TC_BOLD " Summary" TC_RST ":-\n");
	obj = json_object_get(result, "summary");
	print_json_tree(obj, bread_crumb, 0, NULL);

	memset(bread_crumb, 0, sizeof(char *) * MAX_BREAD_CRUMB_LVL);
	printf(TC_BOLD " Details" TC_RST ":-\n");
	obj = json_object_get(result, "detail");
	print_json_tree(obj, bread_crumb, 0, NULL);

	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

static int display_calulated_income_tax_nics(const char *cid)
{
	json_t *result;
	json_t *obj;
	char *jbuf;
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = { NULL };
	int ret = -1;
	int err;

	err = mtd_ic_sa_get_income_tax_nics_calc(cid, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get income TAX NICs calculation. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	printf(SUCCESS
	       "Income TAX NICs calculation for " TC_BOLD "%s" TC_RST "\n",
	       cid);

	result = get_result_json(jbuf);

	printf(TC_BOLD " Summary" TC_RST ":-\n");
	obj = json_object_get(result, "summary");
	print_json_tree(obj, bread_crumb, 0, NULL);

	memset(bread_crumb, 0, sizeof(char *) * MAX_BREAD_CRUMB_LVL);
	printf(TC_BOLD " Details" TC_RST ":-\n");
	obj = json_object_get(result, "detail");
	print_json_tree(obj, bread_crumb, 0, NULL);

	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

static int display_taxable_income(const char *cid)
{
	json_t *result;
	json_t *obj;
	char *jbuf;
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = { NULL };
	int ret = -1;
	int err;

	err = mtd_ic_sa_get_taxable_income(cid, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get taxable income. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	printf(SUCCESS
	       "Taxable Income for calculation " TC_BOLD "%s" TC_RST "\n",
	       cid);

	result = get_result_json(jbuf);

	printf(TC_BOLD " Summary" TC_RST ":-\n");
	obj = json_object_get(result, "summary");
	print_json_tree(obj, bread_crumb, 0, NULL);

	memset(bread_crumb, 0, sizeof(char *) * MAX_BREAD_CRUMB_LVL);
	printf(TC_BOLD " Details" TC_RST ":-\n");
	obj = json_object_get(result, "detail");
	print_json_tree(obj, bread_crumb, 0, NULL);

	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

#define NO_EST	0x01	/* Don't display the End-of-Year estimate */
static int display_individual_calculations(const char *cid, int flags)
{
	int err;

	JKEY_FW = 46;

	err = display_taxable_income(cid);
	if (err)
		return -1;

	err = display_calulated_income_tax_nics(cid);
	if (err)
		return -1;

	err = display_calulated_a_d_r(cid);
	if (err)
		return -1;

	if (flags & NO_EST)
		return 0;
	err = display_end_of_year_est(cid);
	if (err)
		return -1;

	return 0;
}

static int get_calculation_meta(const char *cid)
{
	json_t *result;
	json_t *tyear;
	json_t *ec;
	json_t *value;
	const char *key;
	char *jbuf;
	int ret = -1;
	int err;
	int state;
	int fib_sleep = -1;

again:
	err = mtd_ic_sa_get_calculation_meta(cid, &jbuf);
	if ((err && err != MTD_ERR_REQUEST) ||
	    (err == MTD_ERR_REQUEST && fib_sleep == 5)) {
		fprintf(stderr, "\n" ERROR
			"Couldn't get calculation metadata. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	} else if (err == MTD_ERR_REQUEST) {
		fib_sleep = next_fib(fib_sleep, &state);
		printf("\r  Trying to get calculation metadata again in "
		       TC_BOLD "%d" TC_RST " second(s)", fib_sleep);
		fflush(stdout);
		sleep(fib_sleep);

		goto again;
	}

	result = get_result_json(jbuf);

	ec = json_object_get(result, "calculationErrorCount");
	if (ec && json_integer_value(ec) > 0) {
		fprintf(stderr, ERROR
			"There was a problem with your calculation. Check the "
			"below messages\n        for ERROR's.\n");
		goto out_free_json;
	}

	tyear = json_object_get(result, "taxYear");
	printf(SUCCESS "Calculation Metadata for " TC_BOLD "%s" TC_RST "\n",
	       json_string_value(tyear));

	json_object_foreach(result, key, value) {
		char val[128];

		switch (json_typeof(value)) {
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
		case JSON_OBJECT:
		case JSON_ARRAY:
			continue;
		}

		printf(TC_CHARC "%25s :" TC_RST " %s\n", key, val);
	}

	ret = 0;

out_free_json:
	json_decref(result);

out_free:
	free(jbuf);

	return ret;
}

static int crystallise(int argc, char *argv[])
{
	json_t *result;
	json_t *cid_obj;
	ac_jsonw_t *json = NULL;
	struct mtd_dsrc_ctx dsctx;
	char *jbuf;
	char *s;
	const char *cid;
	char tyear[TAX_YEAR_SZ + 1];
	char submit[32];
	int ret = -1;
	int calc_err;
	int err;

	if (argc < 3) {
		disp_usage();
		return -1;
	}

	snprintf(tyear, sizeof(tyear), "%s", argv[2]);

	err = mtd_ic_cr_intent_to_crystallise(tyear, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Intent to Crystallise failed. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	result = get_result_json(jbuf);
	cid_obj = json_object_get(result, "calculationId");
	cid = json_string_value(cid_obj);

	printf(SUCCESS "Intent to Crystallise calculationId: "
	       TC_BOLD "%s" TC_RST "\n", cid);

	if (!is_prod_api)
		extra_hdrs[0] = "Gov-Test-Scenario: CRYSTALLISATION_METADATA";
	calc_err = get_calculation_meta(cid);
	if (!calc_err) {
		err = display_individual_calculations(cid, NO_EST);
		if (err)
			goto out_free_json;
	}
	err = display_calculation_messages(cid);
	if (calc_err || err)
		goto out_free_json;
	*extra_hdrs = NULL;

	printf("\n");
	printf(CRYSTALLISATION_DECLARATION);
	printf("\n");
	printf(CONFIRM "Crystallise this TAX return? (y/N)> ");

	s = fgets(submit, sizeof(submit), stdin);
	if (!s || (*submit != 'y' && *submit != 'Y'))
		goto out_ok;

	printf("\n");
	printf(INFO "About to " TC_TANG "crystallise" TC_RST " for "
	       TC_BOLD "%s" TC_RST "\n\n", tyear);
	printf(INFO "As a final check measure, just enter 'i agree' at the\n");
	printf(INFO "prompt. Anything else will abandon the process.\n");
	printf("\n");
	printf(CONFIRM "Enter (without the quotes) 'i agree'> ");

	s = fgets(submit, sizeof(submit), stdin);
	if (!s || strcmp(submit, "i agree\n") != 0)
		goto out_ok;

	free(jbuf);

	json = ac_jsonw_init();
	ac_jsonw_set_indenter(json, "    ");
	ac_jsonw_add_str(json, "calculationId", cid);
	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	err = mtd_ic_cr_crystallise(&dsctx, tyear, &jbuf);
	if (err) {
		fprintf(stderr, ERROR "Failed to crystallise. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_json;
	}

	printf(SUCCESS "Crystallisation done.\n");

out_ok:
	ret = 0;

out_free_json:
	json_decref(result);
	ac_jsonw_free(json);

out_free_buf:
	free(jbuf);

	return ret;
}

static int submit_eop_obligation(const char *start, const char *end)
{
	ac_jsonw_t *json;
	struct mtd_dsrc_ctx dsctx;
	char *jbuf;
	char *s;
	char submit[3];
	int ret = -1;
	int err;

	printf(CONFIRM
	       "Submit End of Period Statement for "
	       TC_BOLD "%s" TC_RST " to " TC_BOLD "%s" TC_RST "\n\n"
	       CONFIRM "(y/N)> ", start, end);

	s = fgets(submit, sizeof(submit), stdin);
	if (!s || (*submit != 'y' && *submit != 'Y'))
		return 0;

	json = ac_jsonw_init();
	ac_jsonw_set_indenter(json, "    ");
	ac_jsonw_add_bool(json, "finalised", true);
	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	err = mtd_sa_se_submit_end_of_period_statement(&dsctx, SEID, start,
						       end, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't submit End of Period Statement. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "End of Period Statement submitted for " TC_BOLD "%s"
	       TC_RST " to " TC_BOLD "%s" TC_RST "\n", start, end);

	ret = 0;

out_free:
	ac_jsonw_free(json);
	free(jbuf);

	return ret;
}

static int get_eop_obligations(void)
{
	json_t *result;
	json_t *obs;
	json_t *period;
	char *jbuf;
	size_t index;
	int ret = -1;
	int err;

	err = mtd_sa_se_get_end_of_period_obligations(SEID, NULL, &jbuf);
	if (err) {
		fprintf(stderr, ERROR
			"Couldn't get End of Period Statement(s). (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "End of Period Statement Obligations\n");

        result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");

	printf(TC_CHARC "  %12s %11s %13s %15s %7s" TC_RST "\n",
	       "start", "end", "due", "status", "@" );
	printf(TC_CHARC
	       " ------------------------------------------------------------"
	       "---------" TC_RST "\n");
	json_array_foreach(obs, index, period) {
		json_t *start_obj = json_object_get(period, "start");
		json_t *end_obj = json_object_get(period, "end");
		json_t *due_obj = json_object_get(period, "due");
		json_t *processed = json_object_get(period, "processed");
		const char *start = json_string_value(start_obj);
		const char *end = json_string_value(end_obj);
		const char *due = json_string_value(due_obj);
		bool met = processed ? true : false;

		printf("%s  %15s %12s %13s %9s%s" TC_HI_GREEN "%15s"
		       TC_RST "\n",
		       get_period_color(start, end, due, met),
		       start, end, due, processed ? "F" : "O",
		       TC_RST,
		       processed ? json_string_value(processed) : "");
        }

	json_decref(result);

	ret = 0;

out_free:
	free(jbuf);

	return ret;
}

static int biss_se_summary(const char *tax_year)
{
	json_t *result;
	json_t *obj;
	json_t *val;
	char *jbuf;
	const char *key;
	char *plcolor = TC_GREEN;
	char *pltext = "Profit";
	char qs[56] = "\0";
	int ret = -1;
	int err;

	snprintf(qs, sizeof(qs), "?selfEmploymentId=%s&taxYear=%s", SEID,
		 tax_year);

	err = mtd_biss_get_self_employment(qs, &jbuf);
	if (err) {
		fprintf(stderr, ERROR
			"Couldn't get BISS Self-Employment Annual Summary. "
			"(%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "BISS Self-Employment Annual Summary for "
	       TC_BOLD "%s" TC_RST TC_CHARC " / " TC_RST TC_BOLD "%s" TC_RST
	       "\n", SEID, tax_year);

	result = get_result_json(jbuf);

	printf(TC_BOLD " Total" TC_RST ":-\n");
	obj = json_object_get(result, "total");
	json_object_foreach(obj, key, val)
		printf(TC_CHARC "%23s :" TC_RST " %.2f\n", key,
		       json_number_value(val));

	printf("\n");
	obj = json_object_get(result, "accountingAdjustments");
	printf(TC_CHARC "%23s :" TC_RST " %.2f\n", "accountingAdjustments",
	       json_number_value(obj));

	printf("\n");
	obj = json_object_get(result, "profit");
	if (!obj) {
		obj = json_object_get(result, "loss");
		plcolor = TC_RED;
		pltext = "Loss";
	}
	printf("%s %s" TC_RST ":-\n", plcolor, pltext);
	json_object_foreach(obj, key, val)
		printf(TC_CHARC "%23s :" TC_RST " %.2f\n", key,
		       json_number_value(val));

	json_decref(result);

	ret = 0;

out_free:
	free(jbuf);

	return ret;
}

static const struct class4_nic_ecode_map {
	const char *exempt_code;
	const char *desc;
} class4_nic_ecode_map[] = {
	{ NULL,  NULL },
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
	printf(TC_CHARC " %*s :" TC_RST " %s (%s)\n", JKEY_FW, key, code,
	       class4_nic_ecode_map[atoi(code)].desc);

	return true;
}

static int disp_annual_summary(json_t *root)
{
	const char *bread_crumb[MAX_BREAD_CRUMB_LVL + 1] = { NULL };

	if (!root)
		return -1;

	JKEY_FW = 36;
	print_json_tree(root, bread_crumb, 0, print_c4nic_excempt_type);

	return 0;
}

static int trigger_calculation(const char *tax_year)
{
	json_t *result;
	json_t *cid_obj;
	ac_jsonw_t *json;
	struct mtd_dsrc_ctx dsctx;
	char *jbuf;
	char *s;
	const char *cid;
	int ret = -1;
	int calc_err;
	int err;

	json = ac_jsonw_init();
	ac_jsonw_set_indenter(json, "    ");

	ac_jsonw_add_str(json, "taxYear", tax_year);
	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	err = mtd_ic_sa_trigger_calculation(&dsctx, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't trigger calculation. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "Triggered calculation for " TC_BOLD "%s" TC_RST "\n",
	       tax_year);

	result = get_result_json(jbuf);
	cid_obj = json_object_get(result, "id");
	cid = json_string_value(cid_obj);

	calc_err = get_calculation_meta(cid);
	if (!calc_err) {
		char submit[3];

		printf("\n" CONFIRM "Display full calculation? (y/N)> ");
		s = fgets(submit, sizeof(submit), stdin);
		if (s && (*submit == 'y' || *submit == 'Y')) {
			err = display_individual_calculations(cid, 0);
			if (err)
				goto out_free_json;
		}
	}
	err = display_calculation_messages(cid);
	if (calc_err || err)
		goto out_free_json;

	ret = 0;

out_free_json:
	json_decref(result);

out_free:
	ac_jsonw_free(json);
	free(jbuf);

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

static int annual_summary(const char *tax_year)
{
	json_t *result;
	char *jbuf;
	char *s;
	char tpath[PATH_MAX];
	char submit[3] = "\0";
	int tmpfd;
	int ret = -1;
	int err;

	err = mtd_sa_se_get_annual_summary(SEID, tax_year, &jbuf);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		fprintf(stderr,
			ERROR "Couldn't get Annual Summary. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "Annual Summary for " TC_BOLD "%s" TC_RST "\n",
	       tax_year);

	snprintf(tpath, sizeof(tpath), "/tmp/.itsa_annual_summary.tmp.%d.json",
		 getpid());
	tmpfd = open(tpath, O_CREAT|O_TRUNC|O_RDWR|O_EXCL, 0666);
	if (tmpfd == -1) {
		fprintf(stderr,
			ERROR "Couldn't open %s in %s\n", tpath, __func__);
		perror("open");
		goto out_free;
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
	printf("\n" CONFIRM "Submit (s), Edit (e), Quit (Q)> ");
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
		err = mtd_sa_se_update_annual_summary(&dsctx, SEID, tax_year,
						      &jbuf);
		if (err) {
			fprintf(stderr,
				ERROR "Couldn't update Annual Summary. "
				"(%s)\n%s\n", mtd_err2str(err), jbuf);
			goto out_free_json;
		}

		printf(SUCCESS "Updated Annual Summary for " TC_BOLD "%s"
		       TC_RST "\n", tax_year);

		err = trigger_calculation(tax_year);
		if (err)
			goto out_free_json;

		ret = 0;
		break;
	}
	case 'e':
	case 'E': {
		const char *args[3] = { NULL };
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
			fprintf(stderr,
				ERROR "ftruncate failed in %s\n", __func__);
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

out_free:
	free(jbuf);

	return ret;
}

static int submit_eop_statement(int argc, char *argv[])
{
	char *start;
	char *end;
	char *s;
	char submit[3];
	char tax_year[TAX_YEAR_SZ + 1];
	int err;

	if (argc < 4) {
		disp_usage();
		return -1;
	}

	start = argv[2];
	end = argv[3];

	memcpy(tax_year, start, 4);
	memcpy(tax_year + 4, "-", 2);
	memcpy(tax_year + 5, end + 2, 2);
	tax_year[TAX_YEAR_SZ] = '\0';

	err = annual_summary(tax_year);
	if (err)
		return -1;

	err = biss_se_summary(tax_year);
	if (err)
		return -1;

	printf("\n" EOP_DECLARATION, tax_year);
	printf("\n" CONFIRM "(y/N)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || (*submit != 'y' && *submit != 'Y'))
		return 0;

	err = submit_eop_obligation(start, end);
	if (err)
		return -1;

	return 0;
}

static int update_annual_summary(int argc, char *argv[])
{
	if (argc < 3) {
		disp_usage();
		return -1;
	}

	return annual_summary(argv[2]);
}

static int set_period(const char *start, const char *end, long income,
		      long expenses, enum period_action action)
{
	ac_jsonw_t *json;
	char *jbuf;
	struct mtd_dsrc_ctx dsctx;
	int err;
	int ret = 0;

	json = ac_jsonw_init();
	ac_jsonw_set_indenter(json, "    ");

	ac_jsonw_add_str(json, "from", start);
	ac_jsonw_add_str(json, "to", end);

	ac_jsonw_add_object(json, "incomes");
	ac_jsonw_add_object(json, "turnover");
	ac_jsonw_add_real(json, "amount", income / 100.0f, 2);
	ac_jsonw_end_object(json);
	ac_jsonw_end_object(json);

	ac_jsonw_add_real(json, "consolidatedExpenses", expenses / 100.0f, 2);

	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	if (action == PERIOD_CREATE) {
		err = mtd_sa_se_create_period(&dsctx, SEID, &jbuf);
	} else {
		char period_id[32];

		snprintf(period_id, sizeof(period_id), "%s_%s", start, end);
		err = mtd_sa_se_update_period(&dsctx, SEID, period_id, &jbuf);
	}
	if (err) {
		fprintf(stderr, ERROR "Failed to %s period. (%s)\n%s\n",
			action == PERIOD_CREATE ? "create" : "update",
			mtd_err2str(err), jbuf);
		ret = -1;
	} else {
		printf("\n" SUCCESS "%s period for " TC_BOLD "%s" TC_RST
		       " to " TC_BOLD "%s" TC_RST "\n",
		       action == PERIOD_CREATE ? "Created" : "Updated",
		       start, end);
	}

	ac_jsonw_free(json);
	free(jbuf);

	return ret;
}

static int view_end_of_year_estimate(void)
{
	json_t *result;
	json_t *obs;
	char *jbuf;
	const char *cid = NULL;
	char tyear[TAX_YEAR_SZ + 1];
	char qs[20] = "\0";
	size_t nr_calcs;
	int err;
	int ret = -1;

	get_tax_year(NULL, tyear);

	snprintf(qs, sizeof(qs), "?taxYear=%s", tyear);
	err = mtd_ic_sa_list_calculations(qs, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get calculations list. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "calculations");
	nr_calcs = json_array_size(obs);
	while (nr_calcs--) {
		json_t *calc = json_array_get(obs, nr_calcs);
		json_t *type = json_object_get(calc, "type");
		json_t *cid_obj;

		if (strcmp(json_string_value(type), "inYear") != 0)
			continue;

		cid_obj = json_object_get(calc, "id");
		cid = json_string_value(cid_obj);

		break;
	}

	if (!cid) {
		fprintf(stderr,
			ERROR "No inYear calculation found for "
			TC_BOLD "%s" TC_RST "\n", tyear);
		goto out_free_json;
	}

	printf(SUCCESS
	       "Found inYear calculation for " TC_BOLD "%s" TC_RST "\n",
	       tyear);
	JKEY_FW = 32;
	err = display_end_of_year_est(cid);
	if (err == -RULE_CALCULATION_ERROR_MESSAGES_EXIST) {
		display_calculation_messages(cid);
		goto out_free_json;
	}

	ret = 0;

out_free_json:
	json_decref(result);

out_free_buf:
	free(jbuf);

	return ret;
}

static int list_calculations(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *calculation;
	char *jbuf;
	char *s;
	char qs[20] = "\0";
	char submit[4];
	const char *cid;
	size_t index;
	ac_slist_t *calcs = NULL;
	int err;
	int ret = -1;

	if (argc == 3)
		snprintf(qs, sizeof(qs), "?taxYear=%s", argv[2]);

	err = mtd_ic_sa_list_calculations(qs, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get calculations list. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free_buf;
	}

	printf(SUCCESS "Got list of calculations\n");

	result = get_result_json(jbuf);
	obs = json_object_get(result, "calculations");

	printf(TC_CHARC "  %3s  %24s %25s %14s" TC_RST "\n",
	       "idx", "calculation_id", "timestamp", "type");
	printf(TC_CHARC
	       " ------------------------------------------------------------"
	       "-----------------" TC_RST "\n");
	json_array_foreach(obs, index, calculation) {
		json_t *id_obj = json_object_get(calculation, "id");
		json_t *ts_obj = json_object_get(calculation,
						 "calculationTimestamp");
		json_t *type = json_object_get(calculation, "type");
		const char *id = json_string_value(id_obj);
		const char *ts = json_string_value(ts_obj);
		char date[11];
		char stime[6];

		memcpy(date, ts, 10);
		date[10] = '\0';
		memcpy(stime, ts + 11, 5);
		stime[5] = '\0';
		printf("  " TC_BOLD "%2zu" TC_RST "  %36s %11s %s %16s\n",
		       index + 1, id, date, stime, json_string_value(type));

		ac_slist_add(&calcs, strdup(id));
        }

	printf("\n" CONFIRM "Select a calculation to view (n) or quit (Q)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit < '1' || *submit > '9')
		goto out_free_json;

	index = atoi(submit) - 1;
	cid = ac_slist_nth_data(calcs, index);
	get_calculation_meta(cid);
	display_individual_calculations(cid, 0);
	display_calculation_messages(cid);

out_free_json:
	ac_slist_destroy(&calcs, free);
	json_decref(result);

	ret = 0;

out_free_buf:
	free(jbuf);

	return ret;
}

static int __period_update(const char *start, const char *end,
			   enum period_action action)
{
	long income;
	long expenses;
	int err;
	char *s;
	char tyear[TAX_YEAR_SZ + 1];
	char submit[3];

	get_data(start, end, &income, &expenses);

	printf(CONFIRM "Submit? (y/N)> ");
	s = fgets(submit, sizeof(submit), stdin);
        if (!s || (*submit != 'y' && *submit != 'Y'))
                return 0;

	err = set_period(start, end, income, expenses, action);
	if (err)
		return -1;

	get_tax_year(start, tyear);
	err = trigger_calculation(tyear);
	if (err)
		return -1;

	return 0;
}

static int update_period(int argc, char *argv[])
{
	int err;
	char start[11];
	char end[11];

	if (argc != 3) {
		disp_usage();
		return -1;
	}

	memcpy(start, argv[2], 10);
	start[10] = '\0';
	memcpy(end, argv[2] + 11, 10);
	end[10] = '\0';

	err = __period_update(start, end, PERIOD_UPDATE);
	if (err)
		return -1;

	return 0;
}

static void get_period(char **start, char **end)
{
	json_t *result;
	json_t *obs;
	json_t *period;
	size_t index;
	char *jbuf;
	int err;

	err = mtd_sa_se_list_obligations(SEID, NULL, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get list of obligations. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");
	json_array_foreach(obs, index, period) {
                json_t *met;
		json_t *start_obj;
		json_t *end_obj;

		met = json_object_get(period, "met");
		if (json_is_true(met))
			continue;

		start_obj = json_object_get(period, "start");
		end_obj = json_object_get(period, "end");
		*start = strdup(json_string_value(start_obj));
		*end = strdup(json_string_value(end_obj));

		break;
        }
	json_decref(result);

out_free:
	free(jbuf);
}

static int create_period(void)
{
	char *start = NULL;
	char *end = NULL;
	int ret = 0;
	int err;

	get_period(&start, &end);
	if (!start || !end)
		return -1;

	err = __period_update(start, end, PERIOD_CREATE);
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
	char qs[32] = "\0";
	int err;
	size_t index;
	char *jbuf;

	if (argc > 2 && argc < 4) {
		disp_usage();
		return -1;
	}

	if (argc > 2) {
		int len;

		len = snprintf(qs, sizeof(qs), "?from=%s", argv[2]);
		snprintf(qs + len, sizeof(qs) - len, "&to=%s", argv[3]);
	}

	err = mtd_sa_se_list_obligations(SEID, qs, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get list of obligations. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		free(jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "obligations");

	printf(TC_CHARC "  %14s %18s %11s %12s %8s" TC_RST "\n",
	       "period_id", "start", "end", "due", "met" );
	printf(TC_CHARC
	       " ------------------------------------------------------------"
	       "---------" TC_RST "\n");
	json_array_foreach(obs, index, period) {
		json_t *start_obj = json_object_get(period, "start");
		json_t *end_obj = json_object_get(period, "end");
		json_t *due_obj = json_object_get(period, "due");
		json_t *met = json_object_get(period, "met");
		const char *start = json_string_value(start_obj);
		const char *end = json_string_value(end_obj);
		const char *due = json_string_value(due_obj);

		printf("%s  %s_%-14s %-12s %-12s %-12s%s %s\n",
		       get_period_color(start, end, due, json_is_true(met)),
		       start, end, start, end, due,
		       TC_RST,
		       json_is_true(met) ? STRUE : SFALSE);
        }

	json_decref(result);
	free(jbuf);

	return 0;
}

#define SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS	"A-Za-z0-9 &'()*,-./@£"
#define SAVINGS_ACCOUNT_NAME_REGEX \
	"^[" SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS "]{1,32}$"
static int add_savings_account(void)
{
	char *jbuf;
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

	printf(INFO
	       "Enter a friendly account name, allowed characters are :-\n"
	       "\n\t" TC_BOLD SAVINGS_ACCOUNT_NAME_ALLOWED_CHARS TC_RST "\n");

again:
	printf("\n" CONFIRM "Name> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit == '\n')
		return 0;

	ac_str_chomp(submit);
	ret = regexec(&re, submit, 1, pmatch, 0);
	if (ret != 0) {
		printf(ERROR "Invalid name\n");
		goto again;
	}

	json = ac_jsonw_init();
	ac_jsonw_set_indenter(json, "    ");
	ac_jsonw_add_str(json, "accountName", submit);
	ac_jsonw_end(json);

	dsctx.data_src.buf = ac_jsonw_get(json);
	dsctx.data_len = -1;
	dsctx.src_type = MTD_DATA_SRC_BUF;

	ret = -1;
	err = mtd_sa_sa_create_account(&dsctx, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't add savings account. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	printf(SUCCESS "Added savings account : " TC_BOLD "%s" TC_RST "\n",
	       submit);

	ret = 0;

out_free:
	regfree(&re);
	ac_jsonw_free(json);
	free(jbuf);

	return ret;
}

static int view_savings_accounts(int argc, char *argv[])
{
	json_t *result;
	json_t *obs;
	json_t *account;
	char tyear[TAX_YEAR_SZ + 1];
	char *jbuf;
	size_t index;
	int err;
	int ret = -1;

	err = mtd_sa_sa_list_accounts(&jbuf);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		fprintf(stderr,
			ERROR "Couldn't get list of savings accounts. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		free(jbuf);
		return -1;
	}

	if (argc < 3)
		get_tax_year(NULL, tyear);
	else
		snprintf(tyear, sizeof(tyear), "%s", argv[2]);

	printf(SUCCESS "Savings Accounts for " TC_BOLD "%s" TC_RST "\n",
	       tyear);

	printf("\n" TC_CHARC "  %8s %26s" TC_RST "\n", "id", "name");
	printf(TC_CHARC
	       " ------------------------------------------------------------"
	       TC_RST "\n");

	result = get_result_json(jbuf);
	obs = json_object_get(result, "savingsAccounts");
	free(jbuf);
	jbuf = NULL; /* avoid potential double free if no accounts */
	json_array_foreach(obs, index, account) {
		json_t *res;
		json_t *id = json_object_get(account, "id");
		json_t *name = json_object_get(account, "accountName");
		json_t *taxed_amnt;
		json_t *untaxed_amnt;
		const char *said = json_string_value(id);
		float taxed_int = -1.0;
		float untaxed_int = -1.0;

		err = mtd_sa_sa_get_annual_summary(said, tyear, &jbuf);
		if (err) {
			fprintf(stderr,
				ERROR "Couldn't retrieve account details. "
				"(%s)\n%s\n", mtd_err2str(err), jbuf);
			free(jbuf);
			goto out_free;
		}
		res = get_result_json(jbuf);
		taxed_amnt = json_object_get(res, "taxedUkInterest");
		untaxed_amnt = json_object_get(res, "untaxedUkInterest");

		if (taxed_amnt)
			taxed_int = json_real_value(taxed_amnt);
		if (untaxed_amnt)
			untaxed_int = json_real_value(untaxed_amnt);

		printf("  %-25s %-34s\n",
		       json_string_value(id), json_string_value(name));
		if (taxed_int >= 0.0f)
			printf(TC_CHARC "%25s" TC_RST TC_BOLD "%12.2f" TC_RST
			       "\n", "taxedUkInterest : ", taxed_int);
		if (untaxed_int >= 0.0f)
			printf(TC_CHARC "%25s" TC_RST TC_BOLD "%12.2f" TC_RST
			       "\n","untaxedUkInterest : ", untaxed_int);
		printf("\n");

		json_decref(res);
		free(jbuf);
        }

	ret = 0;

out_free:
	json_decref(result);

	return ret;
}

static int get_savings_accounts_list(ac_slist_t **accounts)
{
	json_t *result;
	json_t *obs;
	json_t *account;
	char *jbuf;
	size_t index;
	int err;

	err = mtd_sa_sa_list_accounts(&jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get list of savings accounts. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		free(jbuf);
		return -1;
	}

	result = get_result_json(jbuf);
	obs = json_object_get(result, "savingsAccounts");

	printf(TC_CHARC "  idx %9s %26s" TC_RST "\n", "id", "name");
	printf(TC_CHARC
	       " ------------------------------------------------------------"
	       "---" TC_RST "\n");
	json_array_foreach(obs, index, account) {
		json_t *id = json_object_get(account, "id");
		json_t *name = json_object_get(account, "accountName");

		printf("  %2zu    %-22s %s\n", index + 1,
		       json_string_value(id), json_string_value(name));

		ac_slist_add(accounts, strdup(json_string_value(id)));
        }

	json_decref(result);
	free(jbuf);

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
	char *jbuf;
	char *s;
	char submit[3];
	char tpath[PATH_MAX];
	const char *tyear;
	const char *said;
	const char *args[3] = { NULL };
	int child_pid;
	int status;
	int tmpfd;
	int ret = -1;
	int err;

	if (argc < 3) {
		disp_usage();
		return -1;
	}

	tyear = argv[2];

	get_savings_accounts_list(&accounts);
	printf("\n" CONFIRM "Select account to edit (n) or quit (Q)> ");
	s = fgets(submit, sizeof(submit), stdin);
	if (!s || *submit < '1' || *submit > '9')
		goto out_free;

	said = ac_slist_nth_data(accounts, atoi(submit) - 1);
	if (!said) {
		fprintf(stderr, ERROR "No such account index\n");
		goto out_free;
	}
	err = mtd_sa_sa_get_annual_summary(said, tyear, &jbuf);
	if (err && mtd_http_status_code(jbuf) != MTD_HTTP_NOT_FOUND) {
		fprintf(stderr,
			ERROR "Couldn't retrieve account details. "
			"(%s)\n%s\n", mtd_err2str(err), jbuf);
		goto out_free;
	}
	if (mtd_http_status_code(jbuf) == MTD_HTTP_NOT_FOUND) {
		fprintf(stderr, ERROR "No such Savings Account\n");
		goto out_free;
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
		fprintf(stderr,
			ERROR "Couldn't open %s in %s\n", tpath, __func__);
		perror("open");
		goto out_free;
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
	err = mtd_sa_sa_update_annual_summary(&dsctx, said, tyear, &jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't update Savings Account. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_close_tmpfd;
	}

	printf(SUCCESS "Updated Savings Account " TC_BOLD "%s" TC_RST "\n",
	       said);
	args[2] = tyear;
	view_savings_accounts(3, (char **)args);

	ret = 0;

out_close_tmpfd:
	close(tmpfd);
	unlink(tpath);

	json_decref(result);

out_free:
	ac_slist_destroy(&accounts, free);
	free(jbuf);

	return ret;
}

static int do_init_all(const struct mtd_cfg *cfg)
{
	json_t *result = NULL;
	json_t *employment;
	json_t *seid;
	json_t *config;
	json_error_t error;
	char *jbuf;
	char path[PATH_MAX];
	int dfd;
	int ret = -1;
	int err;

	/* Quick check to see if we alresdy have a libmtdac config... */
	snprintf(path, sizeof(path), "%s/libmtdac/%s", cfg->config_dir,
		 is_prod_api ? "prod-api" : "test-api");
	dfd = open(path, O_PATH|O_DIRECTORY);
	if (dfd != -1) {
		struct stat sb;

		err = fstatat(dfd, "config.json", &sb, 0);
		if (!err) {
			char *s;
			char submit[3];

			printf(WARNING "Existing libmtdac config found @ %s\n",
			       path);
			printf(CONFIRM "Continue? (y/N)> ");
			s = fgets(submit, sizeof(submit), stdin);
			if (!s || (*submit != 'y' && *submit != 'Y'))
				return 0;
			printf("\n");
		}
	}

	printf("Initialising...\n\n");
	err = mtd_init_config();
	if (err)
		return err;

	printf("\n");
	err = mtd_init_nino();
	if (err)
		return err;

	printf("\n");
	err = mtd_init_auth();
	if (err)
		return err;

	printf(INFO "Looking up SEID...\n");
	err = mtd_sa_se_list_employments(&jbuf);
	if (err) {
		fprintf(stderr,
			ERROR "Couldn't get list of employments. (%s)\n%s\n",
			mtd_err2str(err), jbuf);
		goto out_free;
	}

	result = get_result_json(jbuf);
	employment = json_array_get(result, json_array_size(result) - 1);
	seid = json_object_get(employment, "id");
	if (!seid) {
		printf(ERROR "No employments found\n%s\n", jbuf);
		goto out_free;
	}

	printf(SUCCESS "Found SEID : " TC_BOLD "%s" TC_RST "\n",
	       json_string_value(seid));

	snprintf(path, sizeof(path), "%s/" ITSA_CFG, getenv("HOME"));
	config = json_load_file(path, 0, &error);
	if (!config) {
		fprintf(stderr,
			ERROR "Couldn't open itsa/config.json: %s\n",
			error.text);
		goto out_free;
	}

	json_object_set(config, "self_employment_id", seid);
	json_dump_file(config, path, JSON_INDENT(4));
	json_decref(config);

	printf("\n" SUCCESS
	       "Initialisation complete. Re-run command if something looks "
	       "wrong.\n");

	ret = 0;

out_free:
	free(jbuf);
	json_decref(result);

	return ret;
}

static void print_api_info(void)
{
	printf(INFO "***\n");
	printf(INFO "*** Using %s API\n",
	       is_prod_api ? TC_RED"PRODUCTION"TC_RST : TC_TANG"TEST"TC_RST);
	printf(INFO "***\n\n");
}

static int read_config(void)
{
	json_t *root;
	json_t *prod_api;
	json_t *gnc_obj;
	json_t *seid_obj;
	json_t *nino_obj;
	json_t *client_secrets_obj;
	json_t *client_id_obj;
	json_t *client_secret_obj;
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/" ITSA_CFG, getenv("HOME"));

	root = json_load_file(path, 0, NULL);
	if (!root) {
		printf(ERROR "Unable to open config : %s\n", path);
		return -1;
	}

	prod_api = json_object_get(root, "production_api");
	is_prod_api = json_is_true(prod_api);

	gnc_obj = json_object_get(root, "gnc_sqlite");
	itsa_config.gnc = strdup(json_string_value(gnc_obj));

	seid_obj = json_object_get(root, "self_employment_id");
	itsa_config.seid = seid_obj ? strdup(json_string_value(seid_obj)) :
				      NULL;

	nino_obj = json_object_get(root, "national_insurance_no");
	itsa_config.nino = nino_obj ? strdup(json_string_value(nino_obj)) :
				      NULL;

	client_secrets_obj = json_object_get(root, "client_secrets");

	client_id_obj = json_object_get(client_secrets_obj, "client_id");
	itsa_config.client_id = client_id_obj ?
		strdup(json_string_value(client_id_obj)) : NULL;

	client_secret_obj = json_object_get(client_secrets_obj,
					    "client_secret");
	itsa_config.client_secret = client_secret_obj ?
		strdup(json_string_value(client_secret_obj)) : NULL;

	json_decref(root);

	return 0;
}

static char *set_screens(void *user_data __unused)
{
	struct winsize ws;
	char *buf;
	int len;

	ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
	len = asprintf(&buf,
		       "width=%hu&height=%hu&scaling-factor=1&colour-depth=32",
		       ws.ws_xpixel, ws.ws_ypixel);
	if (len == -1) {
		perror("asprintf");
		buf = NULL;
	}

	return buf;
}

static char *set_win_sz(void *user_data __unused)
{
	struct winsize ws;
	char *buf;
	int len;

	ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
	len = asprintf(&buf, "width=%hu&height=%hu", ws.ws_xpixel,
		       ws.ws_ypixel);
	if (len == -1) {
		perror("asprintf");
		buf = NULL;
	}

	return buf;
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
		perror("asprintf");
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
		fprintf(stderr, ERROR "open: Can't open %s\n", home_dir);
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
	int err = -1;

	if (IS_CMD("init"))
		err = do_init_all(cfg);
	else if (IS_CMD("re-auth"))
		err = mtd_init_auth();
	else if (IS_CMD("list-periods"))
		err = list_periods(argc, argv);
	else if (IS_CMD("create-period"))
		err = create_period();
	else if (IS_CMD("update-period"))
		err = update_period(argc, argv);
	else if (IS_CMD("update-annual-summary"))
		err = update_annual_summary(argc, argv);
	else if (IS_CMD("submit-end-of-period-statement"))
		err = submit_eop_statement(argc, argv);
	else if (IS_CMD("get-end-of-period-statement-obligations"))
		err = get_eop_obligations();
	else if (IS_CMD("crystallise"))
		err = crystallise(argc, argv);
	else if (IS_CMD("list-calculations"))
		err = list_calculations(argc, argv);
	else if (IS_CMD("view-end-of-year-estimate"))
		err = view_end_of_year_estimate();
	else if (IS_CMD("add-savings-account"))
		err = add_savings_account();
	else if (IS_CMD("view-savings-accounts"))
		err = view_savings_accounts(argc, argv);
	else if (IS_CMD("amend-savings-account"))
		err =amend_savings_account(argc, argv);
	else
		disp_usage();

	return err;
}

int main(int argc, char *argv[])
{
	int err;
	int ret = EXIT_SUCCESS;
	unsigned int flags = MTD_OPT_GLOBAL_INIT;
	char config_dir[PATH_MAX];
	const char *log_level = getenv("ITSA_LOG_LEVEL");
	const struct mtd_fph_ops fph_ops = {
		.fph_screens = set_screens,
		.fph_window_sz = set_win_sz,
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

	flags |= MTD_OPT_ACT_DESKTOP_APP_DIRECT;
	err = mtd_init(flags, &cfg);
	if (err) {
		fprintf(stderr, ERROR "%s\n", mtd_err2str(err));
		exit(EXIT_FAILURE);
	}

	err = dispatcher(argc, argv, &cfg);

	mtd_deinit();
	free_config();

	if (err)
		ret = EXIT_FAILURE;

	exit(ret);
}
