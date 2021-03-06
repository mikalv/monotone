// Copyright (C) 2006 Timothy Brownawell <tbrownaw@gmail.com>
//
// This program is made available under the GNU GPL version 2.0 or
// greater. See the accompanying file COPYING for details.
//
// This program is distributed WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE.

#include "base.hh"

#include <csignal>
#include <cstdlib>

#include "lua.hh"
#include "platform.hh"
#include "sanity.hh"

using std::string;
using std::malloc;
using std::free;

LUAEXT(get_ostype, )
{
  std::string str;
  get_system_flavour(str);
  lua_pushstring(LS, str.c_str());
  return 1;
}

LUAEXT(existsonpath, )
{
  const char *exe = luaL_checkstring(LS, -1);
  lua_pushnumber(LS, existsonpath(exe));
  return 1;
}

LUAEXT(is_executable, )
{
  const char *path = luaL_checkstring(LS, -1);
  lua_pushboolean(LS, is_executable(path));
  return 1;
}

LUAEXT(set_executable, )
{
  const char *path = luaL_checkstring(LS, -1);
  lua_pushnumber(LS, set_executable(path));
  return 1;
}

LUAEXT(clear_executable, )
{
  const char *path = luaL_checkstring(LS, -1);
  lua_pushnumber(LS, clear_executable(path));
  return 1;
}

LUAEXT(spawn, )
{
  int n = lua_gettop(LS);
  const char *path = luaL_checkstring(LS, 1);
  char **argv = (char**)malloc((n+1)*sizeof(char*));
  int i;
  pid_t ret;
  if (argv==NULL)
    return 0;
  argv[0] = (char*)path;
  for (i=1; i<n; i++) argv[i] = (char*)luaL_checkstring(LS, i+1);
  argv[i] = NULL;
  ret = process_spawn(argv);
  free(argv);
  lua_pushnumber(LS, ret);
  return 1;
}

LUAEXT(spawn_redirected, )
{
  int n = lua_gettop(LS);
  char const * infile = luaL_checkstring(LS, 1);
  char const * outfile = luaL_checkstring(LS, 2);
  char const * errfile = luaL_checkstring(LS, 3);
  const char *path = luaL_checkstring(LS, 4);
  n -= 3;
  char **argv = (char**)malloc((n+1)*sizeof(char*));
  int i;
  pid_t ret;
  if (argv==NULL)
    return 0;
  argv[0] = (char*)path;
  for (i=1; i<n; i++) argv[i] = (char*)luaL_checkstring(LS,  i+4);
  argv[i] = NULL;
  ret = process_spawn_redirected(infile, outfile, errfile, argv);
  free(argv);
  lua_pushnumber(LS, ret);
  return 1;
}

// Making C functions that return FILE* in Lua is tricky. Especially if it
// actually needs to work with multiple lua versions.
//
// The following routines are inspired by lua/liolib.c from both versions.
// The mtn_lua_Stream struct is closer to the 5.2 variant, but the
// additional field compared to 5.1 (which only uses FILE*) shouldn't hurt
// in Lua 5.1.
//
// There is a Lua FAQ entitled:
// "Why does my library-created file segfault on :close() but work otherwise?"
//
// However, it's advice seems out-dated and applies more to 5.1.

typedef struct mtn_lua_Stream {
  FILE *f;
  lua_CFunction closef;
} mtn_lua_Stream;

#define topfile(LS)     ((mtn_lua_Stream *)luaL_checkudata(LS, 1, LUA_FILEHANDLE))

static int io_fclose (lua_State *LS) {
  mtn_lua_Stream *s = topfile(LS);

  // Note that in Lua 5.2, aux_close() already resets s->closef to NULL and for
  // Lua 5.1, it's not relevant, at all. But we set it to &io_fclose() in both
  // cases, so contents of s->closef differs between Lua versions at this point
  // in the code. However, it's not used, but only reset to NULL.

  int ok = 1;
  if (s->f != NULL)
    ok = (fclose(s->f) == 0);

  s->f = NULL;
  s->closef = NULL;  // just to be extra sure this won't do any harm

  lua_pushboolean(LS, ok);

  return 1;
}

static mtn_lua_Stream *newstream (lua_State *LS) {
  mtn_lua_Stream *s = (mtn_lua_Stream *)lua_newuserdata(LS, sizeof(mtn_lua_Stream));
  s->f = NULL;  /* file handle is currently `closed' */
  s->closef = NULL;
  luaL_getmetatable(LS, LUA_FILEHANDLE);
  lua_setmetatable(LS, -2);

#ifdef LUA_ENVIRONINDEX
  // Lua 5.2 removes C function environments
  lua_pushcfunction(LS, io_fclose);
  lua_setfield(LS, LUA_ENVIRONINDEX, "__close");
#endif

  return s;
}

LUAEXT(spawn_pipe, )
{
  int n = lua_gettop(LS);
  char **argv = (char**)malloc((n+1)*sizeof(char*));
  int i;
  pid_t pid;
  if (argv == NULL)
    return 0;
  if (n < 1)
    {
      free(argv);
      return 0;
    }
  for (i=0; i<n; i++)
    argv[i] = (char*)luaL_checkstring(LS,  i+1);
  argv[i] = NULL;

  mtn_lua_Stream *ins = newstream(LS);
  ins->closef = &io_fclose;

  mtn_lua_Stream *outs = newstream(LS);
  outs->closef = &io_fclose;

  pid = process_spawn_pipe(argv, &ins->f, &outs->f);
  free(argv);

  lua_pushnumber(LS, pid);

  return 3;
}

LUAEXT(wait, )
{
  pid_t pid = static_cast<pid_t>(luaL_checknumber(LS, -1));
  int res;
  int ret;
  ret = process_wait(pid, &res);
  lua_pushnumber(LS, res);
  lua_pushnumber(LS, ret);
  return 2;
}

LUAEXT(kill, )
{
  int n = lua_gettop(LS);
  pid_t pid = static_cast<pid_t>(luaL_checknumber(LS, -2));
  int sig;
  if (n>1)
    sig = static_cast<int>(luaL_checknumber(LS, -1));
  else
    sig = SIGTERM;
  lua_pushnumber(LS, process_kill(pid, sig));
  return 1;
}

LUAEXT(sleep, )
{
  int seconds = static_cast<int>(luaL_checknumber(LS, -1));
  lua_pushnumber(LS, process_sleep(seconds));
  return 1;
}

LUAEXT(get_pid, )
{
  pid_t pid = get_process_id();
  lua_pushnumber(LS, pid);
  return 1;
}

// fs extensions

LUAEXT(mkdir, )
{
  try
    {
      char const * dirname = luaL_checkstring(LS, -1);
      do_mkdir(dirname);
      lua_pushboolean(LS, true);
      return 1;
    }
  catch(recoverable_failure & e)
    {
      lua_pushnil(LS);
      return 1;
    }
}

LUAEXT(exists, )
{
  try
    {
      char const * name = luaL_checkstring(LS, -1);
      lua_pushboolean(LS, get_path_status(name) != path::nonexistent);
    }
  catch(recoverable_failure & e)
    {
      lua_pushnil(LS);
    }
  return 1;
}

LUAEXT(isdir, )
{
  try
    {
      char const * name = luaL_checkstring(LS, -1);
      lua_pushboolean(LS, get_path_status(name) == path::directory);
    }
  catch(recoverable_failure & e)
    {
      lua_pushnil(LS);
    }
  return 1;
}

namespace
{
  struct build_table : public dirent_consumer
  {
    build_table(lua_State * st) : st(st), n(1)
    {
      lua_newtable(st);
    }
    virtual void consume(const char *s)
    {
      lua_pushstring(st, s);
      lua_rawseti(st, -2, n);
      n++;
    }
  private:
    lua_State * st;
    unsigned int n;
  };
}

LUAEXT(read_directory, )
{
  int top = lua_gettop(LS);
  try
    {
      string path(luaL_checkstring(LS, -1));
      build_table tbl(LS);

      read_directory(path, tbl, tbl, tbl);
    }
  catch(recoverable_failure &)
    {
      // discard the table and any pending path element
      lua_settop(LS, top);
      lua_pushnil(LS);
    }
  catch (...)
    {
      lua_settop(LS, top);
      throw;
    }
  return 1;
}

// Local Variables:
// mode: C++
// fill-column: 76
// c-file-style: "gnu"
// indent-tabs-mode: nil
// End:
// vim: et:sw=2:sts=2:ts=2:cino=>2s,{s,\:s,+s,t0,g0,^-2,e-2,n-2,p2s,(0,=s:
