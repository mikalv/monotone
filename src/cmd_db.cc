// Copyright (C) 2002 Graydon Hoare <graydon@pobox.com>
//
// This program is made available under the GNU GPL version 2.0 or
// greater. See the accompanying file COPYING for details.
//
// This program is distributed WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE.

#include "base.hh"
#include <iostream>
#include <set>
#include <utility>

#include "charset.hh"
#include "cmd.hh"
#include "revision.hh"
#include "roster.hh"
#include "constants.hh"
#include "app_state.hh"
#include "database.hh"
#include "project.hh"
#include "keys.hh"
#include "key_store.hh"
#include "work.hh"
#include "rev_height.hh"
#include "transforms.hh"
#include "ui.hh"
#include "vocab_cast.hh"
#include "migration.hh"

using std::cin;
using std::cout;
using std::make_pair;
using std::pair;
using std::set;
using std::string;
using std::vector;

CMD_GROUP(db, "db", "", CMD_REF(database),
          N_("Deals with the database"),
          "");

CMD(db_init, "init", "", CMD_REF(db), "",
    N_("Initializes a database"),
    N_("Creates a new database file and initializes it."),
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.initialize();
}

CMD(db_info, "info", "", CMD_REF(db), "",
    N_("Shows information about the database"),
    "",
    options::opts::full)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.info(cout, app.opts.full);
}

CMD(db_version, "version", "", CMD_REF(db), "",
    N_("Shows the database's version"),
    "",
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.version(cout);
}

CMD(db_fix_certs, "fix_certs", "", CMD_REF(db), "",
    N_("Attempt to fix bad certs"),
    N_("Older monotone versions could sometimes associate certs with "
       "the wrong key. This fixes such certs if you have the correct key, "
       "and can optionally drop any certs that you don't have the "
       "correct key for. This should only be needed if you had such certs "
       "in your db when upgrading from 0.44 or earlier, or if you loaded "
       "such certs with 'mtn read'."),
    options::opts::drop_bad_certs)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.fix_bad_certs(app.opts.drop_bad_certs);
}

CMD(db_dump, "dump", "", CMD_REF(db), "",
    N_("Dumps the contents of the database"),
    N_("Generates a list of SQL instructions that represent the whole "
       "contents of the database.  The resulting output is useful to later "
       "restore the database from a text file that serves as a backup."),
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.dump(cout);
}

CMD(db_load, "load", "", CMD_REF(db), "",
    N_("Loads the contents of the database"),
    N_("Reads a list of SQL instructions that regenerate the contents of "
       "the database.  This is supposed to be used in conjunction with the "
       "output generated by the 'dump' command."),
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  db.load(cin);
}

CMD(db_migrate, "migrate", "", CMD_REF(db), "",
    N_("Migrates the database to a newer schema"),
    N_("Updates the database's internal schema to the most recent one.  "
       "Needed to automatically resolve incompatibilities that may be "
       "introduced in newer versions of monotone."),
    options::opts::none)
{
  key_store keys(app);

  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  migration_status mstat;
  {
    database db(app);
    db.migrate(keys, mstat);
    database::reset_cache();
  }

  if (mstat.need_regen())
    {
      database db(app);
      regenerate_caches(db, mstat.regen_type());
    }

  if (mstat.need_flag_day())
    {
      P(F("NOTE: because this database was last used by a rather old version\n"
          "of monotone, you're not done yet.  If you're a project leader, then\n"
          "see the file UPGRADE for instructions on running '%s db %s'")
        % prog_name % mstat.flag_day_name());
    }
}

CMD(db_execute, "execute", "", CMD_REF(db), "",
    N_("Executes an SQL command on the database"),
    N_("Directly executes the given SQL command on the database"),
    options::opts::none)
{
  if (args.size() != 1)
    throw usage(execid);

  database db(app);
  db.debug(idx(args, 0)(), cout);
}

CMD_GROUP(db_local, "local", "", CMD_REF(database),
          N_("Commands that delete items from the local database"),
          N_("Deletions cannot be propagated through netsync, so the deleted items "
             "will come back if you sync with a database that still has them."));

CMD(db_kill_rev_locally, "kill_revision", "", CMD_REF(db_local), "REVID",
    N_("Kills a revision from the local database"),
    "",
    options::opts::none)
{
  if (args.size() != 1)
    throw usage(execid);

  revision_id revid;

  database db(app);
  project_t project(db);
  complete(app.opts, app.lua, project, idx(args, 0)(), revid);

  // Check that the revision does not have any children
  std::set<revision_id> children;
  db.get_revision_children(revid, children);
  E(!children.size(), origin::user,
    F("revision %s already has children. We cannot kill it.")
    % revid);

  // If we're executing this in a workspace, check if the workspace parent
  // revision is the one to kill. If so, write out the changes made in this
  // particular revision to _MTN/revision to allow the user redo his (fixed)
  // commit afterwards. Of course we can't do this at all if
  //
  // a) the user is currently not inside a workspace
  // b) the user has updated the current workspace to another revision already
  //    thus the working revision is no longer based on the revision we're
  //    trying to kill
  // c) there are uncomitted changes in the working revision of this workspace.
  //    this *eventually* could be handled with a workspace merge scenario, but
  //    is left out for now
  if (workspace::found)
    {
      workspace work(app);
      revision_t old_work_rev;
      work.get_work_rev(old_work_rev);

      for (edge_map::const_iterator i = old_work_rev.edges.begin();
           i != old_work_rev.edges.end(); i++)
        {
          if (edge_old_revision(i) != revid)
            continue;

          E(!work.has_changes(db), origin::user,
            F("cannot kill revision %s,\n"
              "because it would leave the current workspace in an invalid\n"
              "state, from which monotone cannot recover automatically since\n"
              "the workspace contains uncommitted changes.\n"
              "Consider updating your workspace to another revision first,\n"
              "before you try to kill this revision again.")
              % revid);

          P(F("applying changes from %s on the current workspace")
            % revid);

          revision_t new_work_rev;
          db.get_revision(revid, new_work_rev);
          new_work_rev.made_for = made_for_workspace;
          work.put_work_rev(new_work_rev);
          work.maybe_update_inodeprints(db);

          // extra paranoia... we _should_ never run this section twice
          // since a merged workspace would fail early with work.has_changes()
          break;
        }
    }

  db.delete_existing_rev_and_certs(revid);
}

CMD(db_kill_certs_locally, "kill_certs", "", CMD_REF(db_local),
    "SELECTOR CERTNAME [CERTVAL]",
    N_("Deletes the specified certs from the local database"),
    N_("Deletes all certs which are on the given revision(s) and "
       "have the given name and if a value is specified then also "
       "the given value."),
    options::opts::none)
{
  if (args.size() < 2 || args.size() > 3)
    throw usage(execid);

  string selector = idx(args,0)();
  cert_name name = typecast_vocab<cert_name>(idx(args,1));

  database db(app);
  project_t project(db);

  set<revision_id> revisions;
  complete(app.opts, app.lua, project, selector, revisions);


  transaction_guard guard(db);
  set<cert_value> branches;

  if (args.size() == 2)
    {
      L(FL("deleting all certs named '%s' on %d revisions")
        % name % revisions.size());
      for (set<revision_id>::const_iterator r = revisions.begin();
           r != revisions.end(); ++r)
        {
          if (name == branch_cert_name)
            {
              vector<cert> to_delete;
              db.get_revision_certs(*r, name, to_delete);
              for (vector<cert>::const_iterator i = to_delete.begin();
                   i != to_delete.end(); ++i)
                {
                  branches.insert(i->value);
                }
            }
          db.delete_certs_locally(*r, name);
        }
    }
  else
    {
      cert_value value = typecast_vocab<cert_value>(idx(args,2));
      L(FL("deleting all certs with name '%s' and value '%s' on %d revisions")
        % name % value % revisions.size());
      for (set<revision_id>::const_iterator r = revisions.begin();
           r != revisions.end(); ++r)
        {
          db.delete_certs_locally(*r, name, value);
        }
      branches.insert(value);
    }

  for (set<cert_value>::const_iterator i = branches.begin();
       i != branches.end(); ++i)
    {
      db.recalc_branch_leaves(*i);
      set<revision_id> leaves;
      db.get_branch_leaves(*i, leaves);
      if (leaves.empty())
        db.clear_epoch(typecast_vocab<branch_name>(*i));
    }

  guard.commit();
}

CMD(db_check, "check", "", CMD_REF(db), "",
    N_("Does some sanity checks on the database"),
    N_("Ensures that the database is consistent by issuing multiple "
       "checks."),
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  check_db(db);
}

CMD(db_changesetify, "changesetify", "", CMD_REF(db), "",
    N_("Converts the database to the changeset format"),
    "",
    options::opts::none)
{
  database db(app);
  key_store keys(app);
  project_t project(db);

  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  db.ensure_open_for_format_changes();
  db.check_is_not_rosterified();

  // early short-circuit to avoid failure after lots of work
  cache_user_key(app.opts, project, keys, app.lua);

  build_changesets_from_manifest_ancestry(db, keys, project, set<string>());
}

CMD(db_rosterify, "rosterify", "", CMD_REF(db), "",
    N_("Converts the database to the rosters format"),
    "",
    options::opts::attrs_to_drop)
{
  database db(app);
  key_store keys(app);
  project_t project(db);

  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  db.ensure_open_for_format_changes();
  db.check_is_not_rosterified();

  // early short-circuit to avoid failure after lots of work
  cache_user_key(app.opts, project, keys, app.lua);

  build_roster_style_revs_from_manifest_style_revs(db, keys, project,
                                                   app.opts.attrs_to_drop);
}

CMD(db_regenerate_caches, "regenerate_caches", "", CMD_REF(db), "",
    N_("Regenerates the caches stored in the database"),
    "",
    options::opts::none)
{
  E(args.size() == 0, origin::user,
    F("no arguments needed"));

  database db(app);
  regenerate_caches(db, regen_all);
}

CMD_HIDDEN(clear_epoch, "clear_epoch", "", CMD_REF(db), "BRANCH",
    N_("Clears the branch's epoch"),
    "",
    options::opts::none)
{
  if (args.size() != 1)
    throw usage(execid);

  database db(app);
  db.clear_epoch(typecast_vocab<branch_name>(idx(args, 0)));
}

CMD(db_set_epoch, "set_epoch", "", CMD_REF(db), "BRANCH EPOCH",
    N_("Sets the branch's epoch"),
    "",
    options::opts::none)
{
  if (args.size() != 2)
    throw usage(execid);

  E(idx(args, 1)().size() == constants::epochlen, origin::user,
    F("The epoch must be %d characters") % constants::epochlen);

  epoch_data ed(decode_hexenc_as<epoch_data>(idx(args, 1)(), origin::user));
  database db(app);
  db.set_epoch(branch_name(idx(args, 0)(), origin::user), ed);
}

CMD(set, "set", "", CMD_REF(variables), N_("DOMAIN NAME VALUE"),
    N_("Sets a database variable"),
    N_("This command modifies (or adds if it did not exist before) the "
       "variable named NAME, stored in the database, and sets it to the "
       "given value in VALUE.  The variable is placed in the domain DOMAIN."),
    options::opts::none)
{
  if (args.size() != 3)
    throw usage(execid);

  var_domain d = typecast_vocab<var_domain>(idx(args, 0));
  var_name n;
  var_value v;
  n = typecast_vocab<var_name>(idx(args, 1));
  v = typecast_vocab<var_value>(idx(args, 2));

  database db(app);
  db.set_var(make_pair(d, n), v);
}

CMD(unset, "unset", "", CMD_REF(variables), N_("DOMAIN NAME"),
    N_("Unsets a database variable"),
    N_("This command removes the variable NAME from domain DOMAIN, which "
       "was previously stored in the database."),
    options::opts::none)
{
  if (args.size() != 2)
    throw usage(execid);

  var_domain d = typecast_vocab<var_domain>(idx(args, 0));
  var_name n;
  n = typecast_vocab<var_name>(idx(args, 1));
  var_key k(d, n);

  database db(app);
  E(db.var_exists(k), origin::user,
    F("no var with name '%s' in domain '%s'") % n % d);
  db.clear_var(k);
}

CMD(register_workspace, "register_workspace",  "", CMD_REF(variables),
    N_("[WORKSPACE_PATH]"),
    N_("Registers a new workspace for the current database"),
    N_("This command adds WORKSPACE_PATH to the list of `known-workspaces'."),
    options::opts::none)
{
  if (args.size() > 1)
    throw usage(execid);

  E(args.size() == 1 || workspace::found, origin::user,
    F("no workspace given"));

  system_path workspace;
  if (args.size() == 1)
    workspace = system_path(idx(args, 0)(), origin::user);
  else
    get_current_workspace(workspace);

  database db(app);
  db.register_workspace(workspace);
}

CMD(unregister_workspace, "unregister_workspace", "", CMD_REF(variables),
    N_("[WORKSPACE_PATH]"),
    N_("Unregisters an existing workspace for the current database"),
    N_("This command removes WORKSPACE_PATH to the list of `known-workspaces'."),
    options::opts::none)
{
  if (args.size() > 1)
    throw usage(execid);

  E(args.size() == 1 || workspace::found, origin::user,
    F("no workspace given"));

  system_path workspace;
  if (args.size() == 1)
    workspace = system_path(idx(args, 0)(), origin::user);
  else
    get_current_workspace(workspace);

  database db(app);
  db.unregister_workspace(workspace);
}

CMD(cleanup_workspace_list, "cleanup_workspace_list", "", CMD_REF(variables), "",
    N_("Removes all invalid, registered workspace paths for the current database"),
    "",
    options::opts::none)
{
  if (args.size() != 0)
    throw usage(execid);

  vector<system_path> original_workspaces, valid_workspaces;

  database db(app);
  db.get_registered_workspaces(original_workspaces);

  database_path_helper helper(app.lua);

  for (vector<system_path>::const_iterator i = original_workspaces.begin();
       i != original_workspaces.end(); ++i)
    {
      system_path workspace_path(*i);
      if (!directory_exists(workspace_path / bookkeeping_root_component))
        {
          L(FL("ignoring missing workspace '%s'") % workspace_path);
          continue;
        }

      options workspace_opts;
      workspace::get_options(workspace_path, workspace_opts);

      system_path workspace_db_path;
      helper.get_database_path(workspace_opts, workspace_db_path);

      if (workspace_db_path != db.get_filename())
        {
          L(FL("ignoring workspace '%s', expected database %s, "
               "but has %s configured in _MTN/options")
              % workspace_path % db.get_filename() % workspace_db_path);
          continue;
        }

      valid_workspaces.push_back(workspace_path);
    }

  db.set_registered_workspaces(valid_workspaces);
}

CMD(complete, "complete", "", CMD_REF(informative),
    N_("(revision|file|key) PARTIAL-ID"),
    N_("Completes a partial identifier"),
    "",
    options::opts::none)
{
  if (args.size() != 2)
    throw usage(execid);

  database db(app);
  project_t project(db);

  E(idx(args, 1)().find_first_not_of("abcdef0123456789") == string::npos,
    origin::user,
    F("non-hex digits in partial id"));

  if (idx(args, 0)() == "revision")
    {
      set<revision_id> completions;
      db.complete(idx(args, 1)(), completions);
      for (set<revision_id>::const_iterator i = completions.begin();
           i != completions.end(); ++i)
        {
          if (!app.opts.full) cout << *i << '\n';
          else cout << describe_revision(app.opts, app.lua, project, *i) << '\n';
        }
    }
  else if (idx(args, 0)() == "file")
    {
      set<file_id> completions;
      db.complete(idx(args, 1)(), completions);
      for (set<file_id>::const_iterator i = completions.begin();
           i != completions.end(); ++i)
        cout << *i << '\n';
    }
  else if (idx(args, 0)() == "key")
    {
      typedef set< pair<key_id, utf8 > > completions_t;
      completions_t completions;
      db.complete(idx(args, 1)(), completions);
      for (completions_t::const_iterator i = completions.begin();
           i != completions.end(); ++i)
        {
          cout << i->first;
          if (app.opts.full) cout << ' ' << i->second();
          cout << '\n';
        }
    }
  else
    throw usage(execid);
}

CMD_HIDDEN(test_migration_step, "test_migration_step", "", CMD_REF(db),
           "SCHEMA",
           N_("Runs one step of migration on the specified database"),
           N_("This command migrates the given database from the specified "
              "schema in SCHEMA to its successor."),
           options::opts::none)
{
  database db(app);
  key_store keys(app);

  if (args.size() != 1)
    throw usage(execid);
  db.test_migration_step(keys, idx(args,0)());
}

CMD_HIDDEN(rev_height, "rev_height", "", CMD_REF(informative), N_("REV"),
           N_("Shows a revision's height"),
           "",
           options::opts::none)
{
  if (args.size() != 1)
    throw usage(execid);

  revision_id rid(decode_hexenc_as<revision_id>(idx(args, 0)(), origin::user));
  database db(app);
  E(db.revision_exists(rid), origin::user,
    F("no revision %s found in database") % rid);
  rev_height height;
  db.get_rev_height(rid, height);
  P(F("cached height: %s") % height);
}

// loading revisions is relatively fast

CMD_HIDDEN(load_revisions, "load_revisions", "", CMD_REF(db), "",
    N_("Load all revisions from the database"),
    N_("This command loads all revisions from the database and is "
       "intended to be used for timing revision loading performance."),
    options::opts::none)
{
  database db(app);
  set<revision_id> ids;
  vector<revision_id> revisions;

  db.get_revision_ids(ids);
  toposort(db, ids, revisions);

  P(F("loading revisions"));
  ticker loaded(_("revisions"), "r", 64);
  loaded.set_total(revisions.size());

  typedef vector<revision_id>::const_iterator revision_iterator;

  for (revision_iterator i = revisions.begin(); i != revisions.end(); ++i)
    {
      revision_t revision;
      db.get_revision(*i, revision);
      ++loaded;
    }
}

// loading rosters is slow compared with files, revisions or certs

CMD_HIDDEN(load_rosters, "load_rosters", "", CMD_REF(db), "",
    N_("Load all roster versions from the database"),
    N_("This command loads all roster versions from the database and is "
       "intended to be used for timing roster reconstruction performance."),
    options::opts::none)
{
  database db(app);
  set<revision_id> ids;
  vector<revision_id> rosters;

  db.get_revision_ids(ids);
  toposort(db, ids, rosters);

  P(F("loading rosters"));
  ticker loaded(_("rosters"), "r", 1);
  loaded.set_total(rosters.size());
  typedef vector<revision_id>::const_iterator roster_iterator;

  for (roster_iterator i = rosters.begin(); i != rosters.end(); ++i)
    {
      roster_t roster;
      db.get_roster(*i, roster);
      ++loaded;
    }
}

// loading files is slower than revisions but faster than rosters

CMD_HIDDEN(load_files, "load_files", "", CMD_REF(db), "",
    N_("Load all file versions from the database"),
    N_("This command loads all files versions from the database and is "
       "intended to be used for timing file reconstruction performance."),
    options::opts::none)
{
  database db(app);
  set<file_id> files;
  db.get_file_ids(files);

  P(F("loading files"));
  ticker loaded(_("files"), "f", 1);
  loaded.set_total(files.size());

  typedef set<file_id>::const_iterator file_iterator;

  for (file_iterator i = files.begin(); i != files.end(); ++i)
    {
      file_data file;
      db.get_file_version(*i, file);
      ++loaded;
    }
}

// loading certs is fast

CMD_HIDDEN(load_certs, "load_certs", "", CMD_REF(db), "",
    N_("Load all certs from the database"),
    N_("This command loads all certs from the database and is "
       "intended to be used for timing cert loading performance."),
    options::opts::none)
{
  database db(app);
  vector<cert> certs;

  P(F("loading certs"));
  db.get_revision_certs(certs);
  P(F("loaded %d certs") % certs.size());
}

// Local Variables:
// mode: C++
// fill-column: 76
// c-file-style: "gnu"
// indent-tabs-mode: nil
// End:
// vim: et:sw=2:sts=2:ts=2:cino=>2s,{s,\:s,+s,t0,g0,^-2,e-2,n-2,p2s,(0,=s: