// Copyright (C) 2002 Graydon Hoare <graydon@pobox.com>
//
// This program is made available under the GNU GPL version 2.0 or
// greater. See the accompanying file COPYING for details.
//
// This program is distributed WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE.

// this file contains a couple utilities to deal with the user
// interface. the global user_interface object 'ui' owns clog, so no
// writing to it directly!


#include "base.hh"
#include "platform.hh"
#include "paths.hh"
#include "sanity.hh"
#include "ui.hh"
#include "lua.hh"
#include "charset.hh"
#include "simplestring_xform.hh"
#include "constants.hh"
#include "commands.hh"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <map>
#include <set>
#include "lexical_cast.hh"
#include "safe_map.hh"

#include <cstring>

#include "current_exception.hh"

using std::clog;
using std::cout;
using std::endl;
using std::ios_base;
using std::locale;
using std::make_pair;
using std::map;
using std::max;
using std::ofstream;
using std::string;
using std::vector;
using std::set;

using boost::lexical_cast;

struct user_interface ui;

struct user_interface::impl
{
  std::set<string> issued_warnings;

  bool some_tick_is_dirty;    // At least one tick needs being printed
  bool last_write_was_a_tick;
  map<string,ticker *> tickers;
  tick_writer * t_writer;
  string tick_trailer;

  impl() : some_tick_is_dirty(false), last_write_was_a_tick(false),
           t_writer(0) {}
};

ticker::ticker(string const & tickname, string const & s, size_t mod,
    bool kilocount, bool skip_display) :
  ticks(0),
  mod(mod),
  total(0),
  previous_total(0),
  kilocount(kilocount),
  use_total(false),
  may_skip_display(skip_display),
  keyname(tickname),
  name(_(tickname.c_str())),
  shortname(s),
  count_size(0)
{
  I(ui.imp);
  safe_insert(ui.imp->tickers, make_pair(keyname, this));
}

ticker::~ticker()
{
  I(ui.imp);
  safe_erase(ui.imp->tickers, keyname);

  if (ui.imp->some_tick_is_dirty)
    ui.write_ticks();
  ui.finish_ticking();
}

void
ticker::operator++()
{
  I(ui.imp);
  I(ui.imp->tickers.find(keyname) != ui.imp->tickers.end());
  ticks++;
  ui.imp->some_tick_is_dirty = true;
  if (ticks % mod == 0)
    ui.write_ticks();
}

void
ticker::operator+=(size_t t)
{
  I(ui.imp);
  I(ui.imp->tickers.find(keyname) != ui.imp->tickers.end());
  size_t old = ticks;

  ticks += t;
  if (t != 0)
    {
      ui.imp->some_tick_is_dirty = true;
      if (ticks % mod == 0 || (ticks / mod) > (old / mod))
        ui.write_ticks();
    }
}

// We would like to put these in an anonymous namespace but we can't because
// struct user_interface needs to make them friends.
struct tick_writer
{
public:
  tick_writer() {}
  virtual ~tick_writer() {}
  virtual void write_ticks() = 0;
  virtual void clear_line() = 0;
};

struct tick_write_count : virtual public tick_writer
{
public:
  tick_write_count();
  ~tick_write_count();
  void write_ticks();
  void clear_line();
private:
  std::vector<size_t> last_tick_widths;
  size_t last_tick_len;
};

struct tick_write_dot : virtual public tick_writer
{
public:
  tick_write_dot();
  ~tick_write_dot();
  void write_ticks();
  void clear_line();
private:
  std::map<std::string,size_t> last_ticks;
  unsigned int chars_on_line;
};

struct tick_write_stdio : virtual public tick_writer
{
public:
  tick_write_stdio();
  ~tick_write_stdio();
  void write_ticks();
  void clear_line();
private:
  std::map<std::string,size_t> last_ticks;
};

struct tick_write_nothing : virtual public tick_writer
{
public:
  void write_ticks() {}
  void clear_line() {}
};

tick_write_count::tick_write_count() : last_tick_len(0)
{
}

tick_write_count::~tick_write_count()
{
}

static string compose_count(ticker *tick, size_t ticks=0)
{
  string count;

  if (ticks == 0)
    {
      ticks = tick->ticks;
    }

  if (tick->kilocount && ticks)
    {
      // automatic unit conversion is enabled
      float div = 1.0;
      const char *message;

      if (ticks >= 1073741824)
        {
          div = 1073741824;
          // xgettext: gibibytes (2^30 bytes)
          message = N_("%.1f G");
        }
      else if (ticks >= 1048576)
        {
          div = 1048576;
          // xgettext: mebibytes (2^20 bytes)
          message = N_("%.1f M");
        }
      else if (ticks >= 1024)
        {
          div = 1024;
          // xgettext: kibibytes (2^10 bytes)
          message = N_("%.1f k");
        }
      else
        {
          div = 1;
          message = "%.0f";
        }
      // We reset the mod to the divider, to avoid spurious screen updates.
      tick->mod = max(static_cast<int>(div / 10.0), 1);
      count = (F(message) % (ticks / div)).str();
    }
  else if (tick->use_total)
    {
      count = (F("%d/%d") % ticks % tick->total).str();
    }
  else
    {
      // xgettext: bytes
      count = (F("%d") % ticks).str();
    }

  return count;
}

void tick_write_count::write_ticks()
{
  vector<size_t> tick_widths;
  vector<string> tick_title_strings;
  vector<string> tick_count_strings;

  I(ui.imp);
  for (map<string,ticker *>::const_iterator i = ui.imp->tickers.begin();
       i != ui.imp->tickers.end(); ++i)
    {
      ticker * tick = i->second;

      // if the display of this ticker has no great importance, i.e. multiple
      // other tickers should be displayed at the same time, skip its display
      // to save space on terminals
      if (tick->may_skip_display)
        continue;

      if ((tick->count_size == 0 && tick->kilocount)
          ||
          (tick->use_total && tick->previous_total != tick->total))
        {
          if (!tick->kilocount && tick->use_total)
            {
              // We know that we're going to eventually have 'total'
              // displayed twice on screen, plus a slash. So we should
              // pad out this field to that eventual size to avoid
              // spurious re-issuing of the tick titles as we expand to
              // the goal.
              tick->set_count_size(display_width(utf8(compose_count(tick,
                                                                    tick->total),
                                                      origin::internal)));
              tick->previous_total = tick->total;
            }
          else
            {
              // To find out what the maximum size can be, choose one the
              // the dividers from compose_count, subtract one and have
              // compose_count create the count string for that.  Use the
              // size of the returned count string as an initial size for
              // this tick.
              tick->set_count_size(display_width(utf8(compose_count(tick,
                                                                    1048575),
                                                      origin::internal)));
            }
        }

      string count(compose_count(tick));

      size_t title_width = display_width(utf8(tick->name, origin::internal));
      size_t count_width = display_width(utf8(count, origin::internal));

      if (count_width > tick->count_size)
        {
          tick->set_count_size(count_width);
        }

      size_t max_width = max(title_width, tick->count_size);

      string name;
      name.append(max_width - title_width, ' ');
      name.append(tick->name);

      string count2;
      count2.append(max_width - count_width, ' ');
      count2.append(count);

      tick_title_strings.push_back(name);
      tick_count_strings.push_back(count2);
      tick_widths.push_back(max_width);
    }

  string tickline1;
  bool write_tickline1 = !(ui.imp->last_write_was_a_tick
                           && (tick_widths == last_tick_widths));
  if (write_tickline1)
    {
      // Reissue the titles if the widths have changed.
      tickline1 = ui.output_prefix();
      for (size_t i = 0; i < tick_widths.size(); ++i)
        {
          if (i != 0)
            tickline1.append(" | ");
          tickline1.append(idx(tick_title_strings, i));
        }
      last_tick_widths = tick_widths;
      write_tickline1 = true;
    }

  // Always reissue the counts.
  string tickline2 = ui.output_prefix();
  for (size_t i = 0; i < tick_widths.size(); ++i)
    {
      if (i != 0)
        tickline2.append(" | ");
      tickline2.append(idx(tick_count_strings, i));
    }

  if (!ui.imp->tick_trailer.empty())
    {
      tickline2 += " ";
      tickline2 += ui.imp->tick_trailer;
    }

  size_t curr_sz = display_width(utf8(tickline2, origin::internal));
  if (curr_sz < last_tick_len)
    tickline2.append(last_tick_len - curr_sz, ' ');
  last_tick_len = curr_sz;

  unsigned int tw = terminal_width();
  if(write_tickline1)
    {
      if (ui.imp->last_write_was_a_tick)
        clog << '\n';

      if (tw && display_width(utf8(tickline1, origin::internal)) > tw)
        {
          // FIXME: may chop off more than necessary (because we chop by
          // bytes, not by characters)
          tickline1.resize(tw);
        }
      clog << tickline1 << '\n';
    }
  if (tw && display_width(utf8(tickline2, origin::internal)) > tw)
    {
      // FIXME: may chop off more than necessary (because we chop by
      // bytes, not by characters)
      tickline2.resize(tw);
    }
  clog << '\r' << tickline2;
  clog.flush();
}

void tick_write_count::clear_line()
{
  clog << endl;
}


tick_write_dot::tick_write_dot()
{
}

tick_write_dot::~tick_write_dot()
{
}

void tick_write_dot::write_ticks()
{
  I(ui.imp);
  static const string tickline_prefix = ui.output_prefix();
  string tickline1, tickline2;
  bool first_tick = true;

  if (ui.imp->last_write_was_a_tick)
    {
      tickline1 = "";
      tickline2 = "";
    }
  else
    {
      tickline1 = ui.output_prefix() + "ticks: ";
      tickline2 = "\n" + tickline_prefix;
      chars_on_line = tickline_prefix.size();
    }

  for (map<string,ticker *>::const_iterator i = ui.imp->tickers.begin();
       i != ui.imp->tickers.end(); ++i)
    {
      map<string,size_t>::const_iterator old = last_ticks.find(i->first);

      if (!ui.imp->last_write_was_a_tick)
        {
          if (!first_tick)
            tickline1 += ", ";

          tickline1 +=
            i->second->shortname + "=\"" + i->second->name + "\""
            + "/" + lexical_cast<string>(i->second->mod);
          first_tick = false;
        }

      if (old == last_ticks.end()
          || ((i->second->ticks / i->second->mod)
              > (old->second / i->second->mod)))
        {
          chars_on_line += i->second->shortname.size();
          if (chars_on_line > guess_terminal_width())
            {
              chars_on_line = tickline_prefix.size() + i->second->shortname.size();
              tickline2 += "\n" + tickline_prefix;
            }
          tickline2 += i->second->shortname;

          if (old == last_ticks.end())
            last_ticks.insert(make_pair(i->first, i->second->ticks));
          else
            last_ticks[i->first] = i->second->ticks;
        }
    }

  clog << tickline1 << tickline2;
  clog.flush();
}

void tick_write_dot::clear_line()
{
  clog << endl;
}


tick_write_stdio::tick_write_stdio()
{}

tick_write_stdio::~tick_write_stdio()
{}

void tick_write_stdio::write_ticks()
{
  I(ui.imp);
  string headers, sizes, tickline;

  for (map<string,ticker *>::const_iterator i = ui.imp->tickers.begin();
       i != ui.imp->tickers.end(); ++i)
    {
      std::map<std::string, size_t>::iterator it =
            last_ticks.find(i->second->shortname);

      // we output each explanation stanza just once and every time the
      // total count has been changed
      if (it == last_ticks.end())
        {
          headers += i->second->shortname + ":" + i->second->name + ";";
          sizes   += i->second->shortname + "=" +  lexical_cast<string>(i->second->total) + ";";
          last_ticks[i->second->shortname] = i->second->total;
        }
      else
      if (it->second != i->second->total)
        {
          sizes   += i->second->shortname + "=" +  lexical_cast<string>(i->second->total) + ";";
          last_ticks[i->second->shortname] = i->second->total;
        }

      tickline += i->second->shortname + "#" + lexical_cast<string>(i->second->ticks) + ";";
    }

  if (!headers.empty())
    {
      global_sanity.maybe_write_to_out_of_band_handler('t', headers);
    }
  if (!sizes.empty())
    {
      global_sanity.maybe_write_to_out_of_band_handler('t', sizes);
    }

  I(!tickline.empty());
  global_sanity.maybe_write_to_out_of_band_handler('t', tickline);
}

void tick_write_stdio::clear_line()
{
  std::map<std::string, size_t>::iterator it;
  std::string out;

  for (it = last_ticks.begin(); it != last_ticks.end(); it++)
  {
    out += it->first + ";";
  }

  global_sanity.maybe_write_to_out_of_band_handler('t', out);
  last_ticks.clear();
}

// user_interface has both constructor/destructor and initialize/
// deinitialize because there's only one of these objects, it's
// global, and we don't want global constructors/destructors doing
// any real work.  see monotone.cc for how this is handled.

void user_interface::initialize()
{
  imp = new user_interface::impl;

  cout.exceptions(ios_base::badbit);
#ifdef SYNC_WITH_STDIO_WORKS
  clog.sync_with_stdio(false);
#endif
  clog.unsetf(ios_base::unitbuf);
  if (have_smart_terminal())
    set_tick_write_count();
  else
    set_tick_write_dot();

  timestamps_enabled = false;
}

void user_interface::deinitialize()
{
  I(imp);
  delete imp->t_writer;
  delete imp;
}

void
user_interface::finish_ticking()
{
  I(imp);
  if (imp->tickers.empty() && imp->last_write_was_a_tick)
    {
      imp->tick_trailer = "";
      imp->t_writer->clear_line();
      imp->last_write_was_a_tick = false;
    }
}

void
user_interface::set_tick_trailer(string const & t)
{
  I(imp);
  imp->tick_trailer = t;
}

void
user_interface::set_tick_write_dot()
{
  I(imp);
  if (tick_type == dot)
    return;
  if (imp->t_writer != 0)
    delete imp->t_writer;
  imp->t_writer = new tick_write_dot;
  tick_type = dot;
}

void
user_interface::set_tick_write_count()
{
  I(imp);
  if (tick_type == count)
    return;
  if (imp->t_writer != 0)
    delete imp->t_writer;
  imp->t_writer = new tick_write_count;
  tick_type = count;
}

void
user_interface::set_tick_write_stdio()
{
  I(imp);
  if (tick_type == stdio)
    return;
  if (imp->t_writer != 0)
    delete imp->t_writer;
  imp->t_writer = new tick_write_stdio;
  tick_type = stdio;
}

void
user_interface::set_tick_write_nothing()
{
  I(imp);
  if (tick_type == none)
    return;
  if (imp->t_writer != 0)
    delete imp->t_writer;
  imp->t_writer = new tick_write_nothing;
  tick_type = none;
}

user_interface::ticker_type
user_interface::set_ticker_type(user_interface::ticker_type type)
{
  ticker_type ret = tick_type;
  switch (type)
    {
    case count: set_tick_write_count(); break;
    case dot: set_tick_write_dot(); break;
    case stdio: set_tick_write_stdio(); break;
    case none: set_tick_write_nothing(); break;
    }
  return ret;
}

user_interface::ticker_type
user_interface::get_ticker_type() const
{
  return tick_type;
}


void
user_interface::write_ticks()
{
  I(imp);
  imp->t_writer->write_ticks();
  imp->last_write_was_a_tick = true;
  imp->some_tick_is_dirty = false;
}

void
user_interface::warn(string const & warning)
{
  I(imp);
  if (imp->issued_warnings.find(warning) == imp->issued_warnings.end())
    {
      string message;
      prefix_lines_with(_("warning: "), warning, message);
      inform(message);
    }
  imp->issued_warnings.insert(warning);
}

// this message should be kept consistent with unix/main.cc and
// win32/main.cc ::bug_report_message (it is not exactly the same)
void
user_interface::fatal(string const & fatal)
{
  inform(F("fatal: %s\n"
           "This is almost certainly a bug in monotone.\n"
           "Please report this error message, the output of '%s version --full',\n"
           "and a description of what you were doing to '%s'.")
         % fatal % prog_name % PACKAGE_BUGREPORT);
  global_sanity.dump_buffer();
}
// just as above, but the error appears to have come from the database.
// Of course, since the monotone is the only thing that should be
// writing to the database, this still probably means there's a bug.
void
user_interface::fatal_db(string const & fatal)
{
  inform(F("fatal: %s\n"
           "This is almost certainly a bug in monotone.\n"
           "Please report this error message, the output of '%s version --full',\n"
           "and a description of what you were doing to '%s'.\n"
           "This error appears to have been triggered by something in the\n"
           "database you were using, so please preserve it in case it can\n"
           "help in finding the bug.")
         % fatal % prog_name % PACKAGE_BUGREPORT);
  global_sanity.dump_buffer();
}

// Report what we can about a fatal exception (caught in the outermost catch
// handlers) which is from the std::exception hierarchy.  In this case we
// can access the exception object, and we can try to figure out what it
// really is by typeinfo operations.
int
user_interface::fatal_exception(std::exception const & ex)
{
  char const * what = ex.what();
  unrecoverable_failure const * inf;

  if (dynamic_cast<option::option_error const *>(&ex)
      || dynamic_cast<recoverable_failure const *>(&ex))
    {
      this->inform(what);
      return 1;
    }
  else if ((inf = dynamic_cast<unrecoverable_failure const *>(&ex)))
    {
      if (inf->caused_by() == origin::database)
        this->fatal_db(what);
      else
        this->fatal(what);
      return 3;
    }
  else if (dynamic_cast<ios_base::failure const *>(&ex))
    {
      // an error has already been printed
      return 1;
    }
  else if (dynamic_cast<std::bad_alloc const *>(&ex))
    {
      this->inform(_("error: memory exhausted"));
      return 1;
    }
  else // we can at least produce the class name and the what()...
    {
      using std::strcmp;
      using std::strncmp;
      char const * name = typeid(ex).name();
      char const * dem  = demangle_typename(name);

      if (dem == 0)
        dem = name;

      // some demanglers stick "class" at the beginning of their output,
      // which looks dumb in this context
      if (!strncmp(dem, "class ", 6))
        dem += 6;

      // only print what() if it's interesting, i.e. nonempty and different
      // from the name (mangled or otherwise) of the exception type.
      if (what == 0 || what[0] == 0
          || !strcmp(what, name)
          || !strcmp(what, dem))
        this->fatal(dem);
      else
        this->fatal(i18n_format("%s: %s") % dem % what);
      return 3;
    }
}

// Report what we can about a fatal exception (caught in the outermost catch
// handlers) which is of unknown type.  If we have the <cxxabi.h> interfaces,
// we can at least get the type_info object.
int
user_interface::fatal_exception()
{
  std::type_info *type = get_current_exception_type();
  if (type)
    {
      char const * name = type->name();
      char const * dem  = demangle_typename(name);
      if (dem == 0)
        dem = name;
      this->fatal(dem);
    }
  else
    this->fatal(_("C++ exception of unknown type"));
  return 3;
}

string
user_interface::output_prefix()
{
  std::string prefix;

  if (timestamps_enabled) {
    try {
      // To prevent possible infinite loops from a spurious log being
      // made down the line from the call to .as_formatted_localtime,
      // we temporarly turn off timestamping.  Not by fiddling with
      // timestamp_enabled, though, since that one might be looked at
      // by some other code.
      static int do_timestamp = 0;

      if (++do_timestamp == 1) {
        // FIXME: with no app pointer around we have no access to
        // app.lua.get_date_format_spec() here, so we use the same format
        // which f.e. also Apache uses for its log output
        prefix = "[" +
          date_t::now().as_formatted_localtime("%a %b %d %H:%M:%S %Y") +
          "] ";
      }
      --do_timestamp;
    }
    // ensure that we do not throw an exception because we could not
    // create the timestamp prefix above
    catch (...) {}
  }

  if (prog_name.empty()) {
    prefix += "?: ";
  }
  else
  {
    prefix += prog_name + ": ";
  }

  return prefix;
}

static inline string
sanitize(string const & line)
{
  // FIXME: you might want to adjust this if you're using a charset
  // which has safe values in the sub-0x20 range. ASCII, UTF-8,
  // and most ISO8859-x sets do not.
  string tmp;
  tmp.reserve(line.size());
  for (size_t i = 0; i < line.size(); ++i)
    {
      if ((line[i] == '\n')
          || (static_cast<unsigned char>(line[i]) >= static_cast<unsigned char>(0x20)
              && line[i] != static_cast<char>(0x7F)))
        tmp += line[i];
      else
        tmp += ' ';
    }
  return tmp;
}

void
user_interface::ensure_clean_line()
{
  I(imp);
  if (imp->last_write_was_a_tick)
    {
      write_ticks();
      imp->t_writer->clear_line();
    }
  imp->last_write_was_a_tick = false;
}

void
user_interface::redirect_log_to(system_path const & filename)
{
  static ofstream filestr;
  if (filestr.is_open())
    filestr.close();
  filestr.open(filename.as_external().c_str(), ofstream::out | ofstream::app);
  E(filestr.is_open(), origin::system,
    F("failed to open log file '%s'") % filename);
  clog.rdbuf(filestr.rdbuf());
}

void
user_interface::inform(string const & line)
{
  string prefixedLine;
  prefix_lines_with(output_prefix(), line, prefixedLine);
  ensure_clean_line();
  clog << sanitize(prefixedLine) << endl; // flushes
}

unsigned int
guess_terminal_width()
{
  unsigned int w = terminal_width();
  if (!w)
    w = constants::default_terminal_width;
  return w;
}

LUAEXT(guess_terminal_width, )
{
  int w = guess_terminal_width();
  lua_pushinteger(LS, w);
  return 1;
}

// A very simple class that adds an operator() to a string that returns
// the string itself.  This is to make it compatible with, for example,
// the utf8 class, allowing it to be usable in other contexts without
// encoding conversions.
class string_adaptor : public string
{
public:
  string_adaptor(string const & str) : string(str) {}
  string_adaptor(string const & str, origin::type) : string(str) {}
  string const & operator()(void) const { return *this; }
  origin::type made_from;
};

// See description for format_text below for more details.
static vector<string>
wrap_paragraph(string const & text, size_t const line_length,
               size_t const first_line_length)
{
  I(text.find('\n') == string::npos);

  vector<string> wrapped;
  size_t line_len = 0;
  string this_line;

  vector< string_adaptor > words = split_into_words(string_adaptor(text));
  for (vector< string_adaptor >::const_iterator iter = words.begin();
       iter != words.end(); iter++)
    {
      string const & word = (*iter)();
      size_t word_len = display_width(utf8(word, origin::no_fault));
      size_t wanted_len = (wrapped.empty() ? first_line_length : line_length);
      if (iter != words.begin() && line_len + word_len >= wanted_len)
        {
          wrapped.push_back(this_line);
          line_len = 0;
          this_line.clear();
        }
      if (!this_line.empty())
        {
          this_line += " ";
          ++line_len;
        }
      line_len += word_len;
      this_line += word;
    }
  if (!this_line.empty())
    wrapped.push_back(this_line);

  return wrapped;
}

static string
format_paragraph(string const & text, size_t const col,
                 size_t curcol, bool indent_first_line)
{
  string ret;
  size_t const maxcol = guess_terminal_width();
  vector<string> wrapped = wrap_paragraph(text, maxcol - col, maxcol - curcol);
  for (vector<string>::iterator w = wrapped.begin(); w != wrapped.end(); ++w)
    {
      if (w != wrapped.begin())
        ret += "\n";
      if (w != wrapped.begin() || indent_first_line)
        ret += string(col, ' ');
      ret += *w;
    }
  return ret;
}

// Reformats the given text so that it fits in the current screen with no
// wrapping.
//
// The input text is a series of words and sentences.  Paragraphs may be
// separated with a '\n' character, which is taken into account to do the
// proper formatting.  The text should not finish in '\n'.
//
// 'col' specifies the column where the text will start and 'curcol'
// specifies the current position of the cursor.
string
format_text(string const & text, size_t const col,
            size_t curcol, bool indent_first_line)
{
  I(curcol <= col);

  string formatted;

  vector< string > lines;
  split_into_lines(text, lines);
  for (vector< string >::const_iterator iter = lines.begin();
       iter != lines.end(); iter++)
    {
      string const & line = *iter;

      formatted += format_paragraph(line, col, curcol, indent_first_line);
      if (iter + 1 != lines.end())
        formatted += "\n\n";
      curcol = 0;
    }

  return formatted;
}

// See description for the other format_text above for more details.
string
format_text(i18n_format const & text, size_t const col,
            size_t curcol, bool indent_first_line)
{
  return format_text(text.str(), col, curcol, indent_first_line);
}

namespace {
  class option_text
  {
    string names;
    string desc;
    vector<string> formatted_names;
    vector<string> formatted_desc;
  public:
    option_text(string const & n, string const & d)
      : names(n), desc(d) { }
    size_t format_names(size_t const width)
    {
      size_t const full_len = display_width(utf8(names, origin::no_fault));
      size_t const slash = names.find('/');
      if (slash == string::npos || full_len <= width)
        {
          formatted_names.push_back(names);
          return full_len;
        }

      formatted_names.push_back(names.substr(0, slash-1));
      formatted_names.push_back(" " + names.substr(slash-1));

      size_t ret = 0;
      for (vector<string>::const_iterator i = formatted_names.begin();
           i != formatted_names.end(); ++i)
        {
          ret = max(ret, display_width(utf8(*i, origin::no_fault)));
        }
      return ret;
    }
    void format_desc(size_t const width)
    {
      formatted_desc = wrap_paragraph(desc, width, width);
    }
    string formatted(size_t pre_indent, size_t space, size_t namelen) const
    {
      string ret;
      string empty;
      size_t const lines = max(formatted_names.size(), formatted_desc.size());
      for (size_t i = 0; i < lines; ++i)
        {
          string const * left = &empty;
          if (i < formatted_names.size())
            left = &formatted_names.at(i);
          string const * right = &empty;
          if (i < formatted_desc.size())
            right = &formatted_desc.at(i);

          ret += string(pre_indent, ' ')
            + *left + string(namelen - left->size(), ' ')
            + string(space, ' ')
            + *right
            + "\n";
        }
      return ret;
    }
  };
}

// Format a block of options and their descriptions.
static string
format_usage_strings(vector<string> const & names,
                     vector<string> const & descriptions)
{
  // "    --long [ -s ] <arg>    description goes here"
  //  ^  ^^                 ^^  ^^                          ^
  //  |  | \    namelen    / |  | \        descwidth       /| <- edge of screen
  //  ^^^^                   ^^^^
  // pre_indent              space
  string result;

  size_t const pre_indent = 2; // empty space on the left
  size_t const space = 2; // space after the longest option, before the description
  size_t const termwidth = guess_terminal_width();
  size_t const desired_namewidth = (termwidth - pre_indent - space) / 3;
  size_t namelen = 0;

  vector<option_text> texts;
  I(names.size() == descriptions.size());
  for (vector<string>::const_iterator n = names.begin(), d = descriptions.begin();
       n != names.end(); ++n, ++d)
    {
      if (n->empty())
        continue;
      texts.push_back(option_text(*n, *d));

      size_t my_name_len = texts.back().format_names(desired_namewidth);
      if (my_name_len > namelen)
        namelen = my_name_len;
    }

  size_t const descindent = pre_indent + namelen + space;
  size_t const descwidth = termwidth - descindent;

  for (vector<option_text>::iterator i = texts.begin();
       i != texts.end(); ++i)
    {
      i->format_desc(descwidth);

      result += i->formatted(pre_indent, space, namelen);
    }

  result += '\n';
  return result;
}

static string
get_usage_str(options::options_type const & optset, options & opts)
{
  vector<string> names;
  vector<string> descriptions;
  unsigned int maxnamelen;

  optset.instantiate(&opts).get_usage_strings(names, descriptions, maxnamelen,
                                              opts.show_hidden_commands);
  return format_usage_strings(names, descriptions);
}

void
user_interface::inform_usage(usage const & u, options & opts)
{
  // we send --help output to stdout, so that "mtn --help | less" works
  // but we send error-triggered usage information to stderr, so that if
  // you screw up in a script, you don't just get usage information sent
  // merrily down your pipes.
  std::ostream & usage_stream = (opts.help ? cout : clog);

  string visibleid;
  if (!u.which.empty())
    visibleid = join_words(vector< utf8 >(u.which.begin() + 1,
                                          u.which.end()))();

  usage_stream << F("Usage: %s [OPTION...] command [ARG...]") %
    prog_name << "\n\n";

  if (u.which.empty())
    usage_stream << get_usage_str(options::opts::globals(), opts);

  // Make sure to hide documentation that's not part of
  // the current command.
  options::options_type cmd_options =
    commands::command_options(u.which);
  if (!cmd_options.empty())
    {
      usage_stream
        << F("Options specific to '%s %s' "
             "(run '%s help' to see global options):")
        % prog_name % visibleid % prog_name
        << "\n\n";
      usage_stream << get_usage_str(cmd_options, opts);
    }

  commands::explain_usage(u.which, opts.show_hidden_commands, usage_stream);
}

bool
user_interface::enable_timestamps(bool enable)
{
  bool ret = timestamps_enabled;
  timestamps_enabled = enable;
  return ret;
}

// Local Variables:
// mode: C++
// fill-column: 76
// c-file-style: "gnu"
// indent-tabs-mode: nil
// End:
// vim: et:sw=2:sts=2:ts=2:cino=>2s,{s,\:s,+s,t0,g0,^-2,e-2,n-2,p2s,(0,=s:
