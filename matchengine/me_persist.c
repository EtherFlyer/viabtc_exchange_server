/*
 * Description: 
 *     History: yang@haipo.me, 2017/04/04, create
 */

# include "me_config.h"
# include "me_persist.h"
# include "me_operlog.h"
# include "me_load.h"
# include "me_dump.h"

static time_t last_slice_time;
static nw_timer timer;

static time_t get_today_start(void)
{
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = lt->tm_year;
    t.tm_mon  = lt->tm_mon;
    t.tm_mday = lt->tm_mday;
    return mktime(&t);
}

static int get_last_slice(MYSQL *conn, time_t *timestamp, uint64_t *id)
{
    sds sql = sdsempty();
    sql = sdscatprintf(sql, "SELECT `time`, `end_id` from `slice_history` ORDER BY `id` DESC LIMIT 1");
    log_stderr("get last slice time");
    log_trace("exec sql: %s", sql);
    int ret = mysql_real_query(conn, sql, sdslen(sql));
    if (ret != 0) {
        log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
        log_stderr("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
        sdsfree(sql);
        return -__LINE__;
    }

    MYSQL_RES *result = mysql_store_result(conn);
    size_t num_rows = mysql_num_rows(result);
    if (num_rows != 1) {
        mysql_free_result(result);
        return 0;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    *timestamp = strtol(row[0], NULL, 0);
    *id = strtoull(row[1], NULL, 0);

    return 0;
}

static int load_slice_from_db(MYSQL *conn, time_t timestamp)
{
    sds table = sdsempty();

    table = sdscatprintf(table, "slice_order_%ld", timestamp);
    log_stderr("load orders from: %s", table);
    int ret = load_orders(conn, table);
    if (ret < 0) {
        log_error("load_orders from %s fail: %d", table, ret);
        log_stderr("load_orders from %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }

    sdsclear(table);
    table = sdscatprintf(table, "slice_market_%ld", timestamp);
    log_stderr("load market from: %s", table);
    ret = load_markets(conn, table);
    if (ret < 0) {
        log_error("load_markets from %s fail: %d", table, ret);
        log_stderr("load_markets from %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }

    sdsclear(table);
    table = sdscatprintf(table, "slice_balance_%ld", timestamp);
    log_stderr("load balance from: %s", table);
    ret = load_balance(conn, table);
    if (ret < 0) {
        log_error("load_markets from %s fail: %d", table, ret);
        log_stderr("load_balance from %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }

    sdsfree(table);
    return 0;
}

static int load_operlog_from_db(MYSQL *conn, time_t date, uint64_t *start_id)
{
    struct tm *t = localtime(&date);
    sds table = sdsempty();
    table = sdscatprintf(table, "operlog_%04d%02d%02d", 1900 + t->tm_year, 1 + t->tm_mon, t->tm_mday);
    log_stderr("load oper log from: %s", table);
    if (!is_table_exists(conn, table)) {
        log_error("table %s not exist", table);
        log_stderr("table %s not exist", table);
        sdsfree(table);
        return 0;
    }

    int ret = load_operlog(conn, table, start_id);
    if (ret < 0) {
        log_error("load_operlog from %s fail: %d", table, ret);
        log_stderr("load_operlog from %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }

    sdsfree(table);
    return 0;
}

int init_from_db(void)
{
    MYSQL *conn = mysql_connect(&settings.db_log);
    if (conn == NULL) {
        log_error("connect mysql fail");
        log_stderr("connect mysql fail");
        return -__LINE__;
    }

    time_t now = time(NULL);
    uint64_t last_oper_id = 0;
    int ret = get_last_slice(conn, &last_slice_time, &last_oper_id);
    if (ret < 0) {
        return ret;
    }

    log_info("last_slice_time: %ld, last_oper_id: %"PRIu64"", last_slice_time, last_oper_id);
    log_stderr("last_slice_time: %ld, last_oper_id: %"PRIu64"", last_slice_time, last_oper_id);

    if (last_slice_time == 0) {
        ret = load_operlog_from_db(conn, now, &last_oper_id);
        if (ret < 0)
            goto cleanup;
    } else {
        ret = load_slice_from_db(conn, last_slice_time);
        if (ret < 0) {
            goto cleanup;
        }

        time_t begin = last_slice_time;
        time_t end = get_today_start() + 86400;
        while (begin < end) {
            ret = load_operlog_from_db(conn, begin, &last_oper_id);
            if (ret < 0) {
                goto cleanup;
            }
            begin += 86400;
        }
    }

    operlog_id_start = last_oper_id;
    log_stderr("load success");
    return 0;

cleanup:
    mysql_close(conn);
    return ret;
}

static int dump_order_to_db(MYSQL *conn, time_t end)
{
    sds table = sdsempty();
    table = sdscatprintf(table, "slice_order_%ld", end);
    log_info("dump order to: %s", table);
    int ret = dump_orders(conn, table);
    if (ret < 0) {
        log_error("dump_orders to %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }
    sdsfree(table);

    return 0;
}

static int dump_market_to_db(MYSQL *conn, time_t end)
{
    sds table = sdsempty();
    table = sdscatprintf(table, "slice_market_%ld", end);
    log_info("dump market to: %s", table);
    int ret = dump_markets(conn, table);
    if (ret < 0) {
        log_error("dump_markets to %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }
    sdsfree(table);

    return 0;
}

static int dump_balance_to_db(MYSQL *conn, time_t end)
{
    sds table = sdsempty();
    table = sdscatprintf(table, "slice_balance_%ld", end);
    log_info("dump balance to: %s", table);
    int ret = dump_balance(conn, table);
    if (ret < 0) {
        log_error("dump_balance to %s fail: %d", table, ret);
        sdsfree(table);
        return -__LINE__;
    }
    sdsfree(table);

    return 0;
}

int update_slice_history(MYSQL *conn, time_t end)
{
    sds sql = sdsempty();
    sql = sdscatprintf(sql, "INSERT INTO `slice_history` (`id`, `time`, `end_id`) VALUES (NULL, %ld, %"PRIu64")", end, operlog_id_start);
    log_info("update slice history to: %ld", end);
    log_trace("exec sql: %s", sql);
    int ret = mysql_real_query(conn, sql, sdslen(sql));
    if (ret < 0) {
        log_error("exec sql: %s fail: %d %s", sql, mysql_errno(conn), mysql_error(conn));
        sdsfree(sql);
        return -__LINE__;
    }
    sdsfree(sql);

    return 0;
}

int dump_to_db(time_t timestamp)
{
    MYSQL *conn = mysql_connect(&settings.db_log);
    if (conn == NULL) {
        log_error("connect mysql fail");
        return -__LINE__;
    }

    log_info("start dump slice, timestamp: %ld", timestamp);

    int ret;
    ret = dump_order_to_db(conn, timestamp);
    if (ret < 0) {
        goto cleanup;
    }

    ret = dump_market_to_db(conn, timestamp);
    if (ret < 0) {
        goto cleanup;
    }

    ret = dump_balance_to_db(conn, timestamp);
    if (ret < 0) {
        goto cleanup;
    }

    ret = update_slice_history(conn, timestamp);
    if (ret < 0) {
        goto cleanup;
    }

    log_info("dump success");
    return 0;

cleanup:
    mysql_close(conn);
    return ret;
}

int make_slice(time_t timestamp)
{
    int pid = fork();
    if (pid < 0) {
        log_error("fork fail: %d", pid);
        return -__LINE__;
    } else if (pid > 0) {
        return 0;
    }

    int ret = dump_to_db(timestamp);
    if (ret < 0) {
        log_error("dump_to_db fail: %d", ret);
    }

    exit(0);
    return 0;
}

static void on_timer(nw_timer *timer, void *privdata)
{
    time_t now = time(NULL);
    if (now - last_slice_time >= settings.slice_interval) {
        make_slice(now);
        last_slice_time = now;
    }
}

int init_persist(void)
{
    nw_timer_set(&timer, 60, true, on_timer, NULL);
    nw_timer_start(&timer);

    return 0;
}
