// aqo_cli: interactive SQL shell and batch runner.
//
//   aqo_cli [db_path] [-c "SQL"] [-f script.sql] [--no-index] [--pool N] [--no-sync]
//
// Dot commands: .help .tables .schema [table] .plan on|off .stats .checkpoint .quit
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "aqo/sql/executor.hpp"

using namespace aqo;
using namespace aqo::sql;

namespace {

void print_result(const ResultSet& r, bool show_plan) {
  if (!r.columns.empty()) {
    std::vector<std::size_t> w(r.columns.size());
    for (std::size_t i = 0; i < w.size(); ++i) w[i] = r.columns[i].size();
    std::vector<std::vector<std::string>> cells;
    for (auto& row : r.rows) {
      cells.emplace_back();
      for (std::size_t i = 0; i < row.size(); ++i) {
        cells.back().push_back(to_string(row[i]));
        w[i] = std::max(w[i], cells.back().back().size());
      }
    }
    auto line = [&] {
      for (auto x : w) std::cout << '+' << std::string(x + 2, '-');
      std::cout << "+\n";
    };
    line();
    for (std::size_t i = 0; i < w.size(); ++i) std::cout << "| " << std::left << std::setw(static_cast<int>(w[i])) << r.columns[i] << ' ';
    std::cout << "|\n";
    line();
    for (auto& row : cells) {
      for (std::size_t i = 0; i < w.size(); ++i) std::cout << "| " << std::left << std::setw(static_cast<int>(w[i])) << row[i] << ' ';
      std::cout << "|\n";
    }
    line();
    std::cout << r.rows.size() << " row(s)";
  } else if (!r.message.empty()) {
    std::cout << r.message;
  } else {
    std::cout << "OK";
  }
  std::cout << "  (" << std::fixed << std::setprecision(3) << r.millis << " ms)\n";
  if (show_plan && !r.plan.empty()) std::cout << "plan: " << r.plan << "\n";
}

bool run_sql(Session& s, const std::string& sql, bool show_plan) {
  try {
    for (auto& r : s.execute_script(sql)) print_result(r, show_plan);
    return true;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return false;
  }
}

void dot_command(Database& db, Session& s, const std::string& line, bool& show_plan, bool& quit) {
  std::istringstream in(line);
  std::string cmd, arg;
  in >> cmd >> arg;
  if (cmd == ".quit" || cmd == ".exit") {
    quit = true;
  } else if (cmd == ".help") {
    std::cout << ".tables | .schema [table] | .plan on|off | .stats | .checkpoint | .quit\n"
                 "SQL: CREATE TABLE t (id INT PRIMARY KEY, x REAL, s TEXT); CREATE INDEX ON t (x);\n"
                 "     INSERT / SELECT (WHERE, GROUP BY, ORDER BY, LIMIT, COUNT/SUM/AVG/MIN/MAX) / UPDATE / DELETE\n"
                 "     BEGIN / COMMIT / ROLLBACK, EXPLAIN <statement>\n";
  } else if (cmd == ".tables") {
    for (auto& n : db.table_names()) std::cout << n << "\n";
  } else if (cmd == ".schema") {
    for (auto& n : db.table_names()) {
      if (!arg.empty() && arg != n) continue;
      const Schema& sc = db.find_table(n)->schema();
      std::cout << "CREATE TABLE " << n << " (";
      for (std::size_t i = 0; i < sc.cols.size(); ++i)
        std::cout << (i ? ", " : "") << sc.cols[i].name << ' ' << type_name(sc.cols[i].type) << (i == 0 ? " PRIMARY KEY" : "");
      std::cout << ");\n";
      for (auto c : sc.indexed) std::cout << "CREATE INDEX ON " << n << " (" << sc.cols[c].name << ");\n";
    }
  } else if (cmd == ".plan") {
    show_plan = arg != "off";
  } else if (cmd == ".stats") {
    auto& e = db.engine();
    auto& bs = e.pool().stats();
    std::cout << "commits=" << e.stats().commits << " aborts=" << e.stats().aborts << " checkpoints=" << e.stats().checkpoints
              << " buffer hit rate=" << std::fixed << std::setprecision(3) << bs.hit_rate() << " evictions=" << bs.evictions
              << " wal bytes=" << (e.wal() ? e.wal()->bytes_appended() : 0) << "\n";
  } else if (cmd == ".checkpoint") {
    if (s.in_transaction()) std::cerr << "finish the open transaction first\n";
    else db.engine().checkpoint();
  } else {
    std::cerr << "unknown command " << cmd << " (try .help)\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  EngineOptions opts;
  opts.path = "aqo.db";
  if (const char* env = std::getenv("AQO_DB_PATH")) opts.path = env;
  if (const char* env = std::getenv("AQO_BUFFER_POOL_PAGES")) opts.pool_frames = std::strtoull(env, nullptr, 10);
  if (const char* env = std::getenv("AQO_SYNC_COMMIT")) opts.sync_commit = std::strcmp(env, "0") != 0;
  SessionOptions sopts;
  std::vector<std::string> commands;
  std::string script;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-c" && i + 1 < argc) commands.emplace_back(argv[++i]);
    else if (a == "-f" && i + 1 < argc) script = argv[++i];
    else if (a == "--no-index") sopts.use_aggregate_index = false;
    else if (a == "--no-sync") opts.sync_commit = false;
    else if (a == "--pool" && i + 1 < argc) opts.pool_frames = std::strtoull(argv[++i], nullptr, 10);
    else if (a[0] != '-') opts.path = a;
    else {
      std::cerr << "usage: aqo_cli [db] [-c SQL] [-f file] [--no-index] [--no-sync] [--pool N]\n";
      return 2;
    }
  }
  try {
    Database db(opts);
    if (db.engine().recovery_stats().losers > 0 || db.engine().recovery_stats().redone > 0)
      std::cerr << "recovered: redo=" << db.engine().recovery_stats().redone << " rolled back txns=" << db.engine().recovery_stats().losers << "\n";
    Session s(db, sopts);
    bool show_plan = false, quit = false;
    int rc = 0;
    if (!script.empty()) {
      std::ifstream f(script);
      if (!f) {
        std::cerr << "cannot read " << script << "\n";
        return 2;
      }
      std::stringstream ss;
      ss << f.rdbuf();
      if (!run_sql(s, ss.str(), show_plan)) rc = 1;
    }
    for (auto& c : commands)
      if (!run_sql(s, c, true)) rc = 1;
    if (script.empty() && commands.empty()) {
      bool tty = true;
      std::string buf, line;
      std::cout << "aqo " << opts.path << "  (.help for help, statements end with ';')\n";
      while (!quit) {
        std::cout << (buf.empty() ? "aqo> " : "...> ") << std::flush;
        if (!std::getline(std::cin, line)) break;
        (void)tty;
        if (buf.empty() && !line.empty() && line[0] == '.') {
          dot_command(db, s, line, show_plan, quit);
          continue;
        }
        buf += line + "\n";
        if (line.find(';') != std::string::npos) {
          run_sql(s, buf, show_plan);
          buf.clear();
        }
      }
    }
    if (s.in_transaction()) s.execute("ROLLBACK");
    db.engine().close();
    return rc;
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
