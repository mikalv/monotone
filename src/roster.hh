// Copyright (C) 2005 Nathaniel Smith <njs@pobox.com>
//               2008 Stephen Leake <stephen_leake@stephe-leake.org>
//
// This program is made available under the GNU GPL version 2.0 or
// greater. See the accompanying file COPYING for details.
//
// This program is distributed WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE.

#ifndef __ROSTER_HH__
#define __ROSTER_HH__

#include "hybrid_map.hh"
#include "paths.hh"
#include "rev_types.hh"
#include "cset.hh" // need full definition of editable_tree

#include <stack>

struct node_id_source
{
  virtual node_id next() = 0;
  virtual ~node_id_source() {}
};

///////////////////////////////////////////////////////////////////

node_id const the_null_node = 0;

inline bool
null_node(node_id n)
{
  return n == the_null_node;
}

template <> void dump(attr_map_t const & val, std::string & out);

enum roster_node_type { node_type_none, node_type_file, node_type_dir };

struct dfs_iter
{
  const_dir_t root;
  std::string curr_path;
  bool return_root;
  bool track_path;
  std::stack< std::pair<const_dir_t, dir_map::const_iterator> > stk;

  dfs_iter(const_dir_t r, bool t);
  bool finished() const;
  std::string const & path() const;
  const_node_t operator*() const;
  void operator++();

private:
  void advance_top();
};

struct node
{
  node();
  node(node const & other) = delete;
  node(node && other) = delete;
  node(node_id i);
  node_id self;
  node_id parent; // the_null_node iff this is a root dir
  path_component name; // the_null_component iff this is a root dir
  attr_map_t attrs;
  roster_node_type type;
  u32 cow_version; // see roster_t::cow_version below

  // need a virtual function to make dynamic_cast work
  virtual node_t clone() = 0;
  virtual ~node() {}
};


struct dir_node
  : public node
{
  dir_node();
  dir_node(node_id i);
  dir_map children;
  bool has_child(path_component const & pc) const;
  node_t get_child(path_component const & pc) const;
  void attach_child(path_component const & pc, node_t child);
  node_t detach_child(path_component const & pc);

  // need a virtual function to make dynamic_cast work
  virtual node_t clone();
  virtual ~dir_node() {}
};


struct file_node
  : public node
{
  file_node();
  file_node(node_id i, file_id const & f);
  file_id content;

  // need a virtual function to make dynamic_cast work
  virtual node_t clone();
  virtual ~file_node() {}
};

inline bool
is_dir_t(const_node_t n)
{
  return n->type == node_type_dir;
}

inline bool
is_file_t(const_node_t n)
{
  return n->type == node_type_file;
}

inline bool
is_root_dir_t(node_t n)
{
  if (is_dir_t(n) && n->name.empty())
    {
      I(null_node(n->parent));
      return true;
    }

  return false;
}

inline dir_t
downcast_to_dir_t(node_t const & n)
{
  dir_t d = std::dynamic_pointer_cast<dir_node, node>(n);
  I(static_cast<bool>(d));
  return d;
}

inline file_t
downcast_to_file_t(node_t const & n)
{
  file_t f = std::dynamic_pointer_cast<file_node, node>(n);
  I(static_cast<bool>(f));
  return f;
}

inline const_dir_t
downcast_to_dir_t(const_node_t const & n)
{
  const_dir_t d = std::dynamic_pointer_cast<dir_node const, node const>(n);
  I(static_cast<bool>(d));
  return d;
}

inline const_file_t
downcast_to_file_t(const_node_t const & n)
{
  const_file_t f = std::dynamic_pointer_cast<file_node const, node const>(n);
  I(static_cast<bool>(f));
  return f;
}

bool
shallow_equal(const_node_t a, const_node_t b,
              bool shallow_compare_dir_children,
              bool compare_file_contents = true);

template <> void dump(node_t const & n, std::string & out);

struct marking
{
  u32 cow_version;
  revision_id birth_revision;
  std::set<revision_id> parent_name;
  std::set<revision_id> file_content;
  std::map<attr_key, std::set<revision_id> > attrs;
  marking();
  marking(marking const & other);
  marking const & operator=(marking const & other);
  bool operator==(marking const & other) const
  {
    return birth_revision == other.birth_revision
      && parent_name == other.parent_name
      && file_content == other.file_content
      && attrs == other.attrs;
  }
};

class marking_map
{
  mutable u32 cow_version;
  typedef cow_trie<node_id, marking_t, 8> map_type;
  map_type _store;
public:
  typedef map_type::key_type key_type;
  typedef map_type::value_type value_type;

  marking_map();
  marking_map(marking_map const & other);
  marking_map const & operator=(marking_map const & other);
  const_marking_t get_marking(node_id nid) const;
  marking_t const & get_marking_for_update(node_id nid);
  void put_marking(node_id nid, marking_t const & m);
  void put_marking(node_id nid, const_marking_t const & m);

  // for roster_delta
  void put_or_replace_marking(node_id nid, const_marking_t const & m);

  typedef map_type::const_iterator const_iterator;
  const_iterator begin() const;
  const_iterator end() const;
  size_t size() const;
  bool contains(node_id nid) const;
  void clear();
  void remove_marking(node_id nid);
};

template <> void dump(std::set<revision_id> const & revids, std::string & out);
template <> void dump(marking_t const & marking, std::string & out);
template <> void dump(marking_map const & marking_map, std::string & out);

class roster_t
{
public:
  roster_t();
  roster_t(roster_t const & other);
  roster_t & operator=(roster_t const & other);
  bool has_root() const;
  bool has_node(file_path const & sp) const;
  bool has_node(node_id nid) const;
  bool is_root(node_id nid) const;
  bool is_attached(node_id nid) const;
private:
  node_t get_node_internal(file_path const & p) const;
public:
  const_node_t get_node(file_path const & sp) const;
  const_node_t get_node(node_id nid) const;
  node_t get_node_for_update(file_path const & sp);
  node_t get_node_for_update(node_id nid);
  void get_name(node_id nid, file_path & sp) const;
  void unshare(node_t & n, bool is_in_node_map = true);
  void replace_node_id(node_id from, node_id to);

  // editable_tree operations
  node_id detach_node(file_path const & src);
  void drop_detached_node(node_id nid);
  node_id create_dir_node(node_id_source & nis);
  void create_dir_node(node_id nid);
  node_id create_file_node(file_id const & content,
                           node_id_source & nis);
  void create_file_node(file_id const & content,
                        node_id nid);
  void attach_node(node_id nid, file_path const & dst);
  void attach_node(node_id nid, node_id parent, path_component name);
  void apply_delta(file_path const & pth,
                   file_id const & old_id,
                   file_id const & new_id);
  void clear_attr(file_path const & path,
                  attr_key const & key);
  void set_attr(file_path const & path,
                attr_key const & key,
                attr_value const & val);
  void set_attr(file_path const & path,
                attr_key const & key,
                std::pair<bool, attr_value> const & val);

  // more direct, lower-level operations, for the use of roster_delta's
  void detach_node(node_id nid);
  void set_content(node_id nid,
                   file_id const & new_id);
  void set_attr_unknown_to_dead_ok(node_id nid,
                                   attr_key const & name,
                                   std::pair<bool, attr_value> const & val);
  void erase_attr(node_id nid,
                  attr_key const & name);

  // misc.

  bool get_attr(file_path const & pth,
                attr_key const & key,
                attr_value & val) const;

  void get_file_details(node_id nid,
                        file_id & fid,
                        file_path & pth) const;

  void extract_path_set(std::set<file_path> & paths) const;

  node_map const & all_nodes() const
  {
    return nodes;
  }

  bool operator==(roster_t const & other) const;

  friend bool equal_shapes(roster_t const & a, roster_t const & b);

  void check_sane(bool temp_nodes_ok=false) const;

  // verify that this roster is sane, and corresponds to the given
  // marking map
  void check_sane_against(marking_map const & marks, bool temp_nodes_ok=false) const;

  void print_to(data & dat,
                marking_map const & mm,
                bool print_local_parts) const;

  void parse_from(basic_io::parser & pa,
                  marking_map & mm);

  dir_t const & root() const
  {
    return root_dir;
  }

private:
  //void do_deep_copy_from(roster_t const & other);
  dir_t root_dir;
  node_map nodes;
  // This is set to a unique value when a roster is created or
  // copied to/from another roster. If a node has the same version
  // as the roster it's in, then it is not shared. Nodes with version
  // zero are also not shared, since that means they haven't been added
  // to a roster yet.
  // A simple refcount isn't good enough, because everything that points
  // to a node in question could also be shared.
  mutable u32 cow_version;
  // This requires some explanation.  There is a particular kind of
  // nonsensical behavior which we wish to discourage -- when a node is
  // detached from some location, and then re-attached at that same location
  // (or similarly, if a new node is created, then immediately deleted -- this
  // is like the previous case, if you think of "does not exist" as a
  // location).  In particular, we _must_ error out if a cset attempts to do
  // this, because it indicates that the cset had something non-normalized,
  // like "rename a a" in it, and that is illegal.  There are two options for
  // detecting this.  The more natural approach, perhaps, is to keep a chunk
  // of state around while performing any particular operation (like cset
  // application) for which we wish to detect these kinds of redundant
  // computations.  The other option is to keep this state directly within the
  // roster, at all times.  In the first case, we explicitly turn on checking
  // when we want it; the the latter, we must explicitly turn _off_ checking
  // when we _don't_ want it.  We choose the latter, because it is more
  // conservative --- perhaps it will turn out that it is _too_ conservative
  // and causes problems, in which case we should probably switch to the
  // former.
  //
  // The implementation itself uses the map old_locations.  A node can be in
  // the following states:
  //   -- attached, no entry in old_locations map
  //   -- detached, no entry in old_locations map
  //      -- create_dir_node, create_file_node put a node into this state
  //      -- a node in this state can be attached, anywhere, but may not be
  //         deleted.
  //   -- detached, an entry in old_locations map
  //      -- detach_node puts a node into this state
  //      -- a node in this state can be attached anywhere _except_ the
  //         (parent, basename) entry given in the map, or may be deleted.
  std::map<node_id, std::pair<node_id, path_component> > old_locations;
  template <typename T> friend void dump(T const & val, std::string & out);
};

extern const roster_t empty_roster;

struct temp_node_id_source
  : public node_id_source
{
  temp_node_id_source();
  virtual node_id next();
  node_id curr;
};

template <> void dump(roster_t const & val, std::string & out);

// adaptor class to enable cset application on rosters.
class editable_roster_base
  : public editable_tree
{
public:
  editable_roster_base(roster_t & r, node_id_source & nis);
  virtual node_id detach_node(file_path const & src);
  virtual void drop_detached_node(node_id nid);
  virtual node_id create_dir_node();
  virtual node_id create_file_node(file_id const & content);
  virtual void attach_node(node_id nid, file_path const & dst);
  virtual void apply_delta(file_path const & pth,
                           file_id const & old_id,
                           file_id const & new_id);
  virtual void clear_attr(file_path const & path,
                          attr_key const & key);
  virtual void set_attr(file_path const & path,
                        attr_key const & key,
                        attr_value const & val);
  virtual void commit();
protected:
  roster_t & r;
  node_id_source & nis;
};

bool
equal_up_to_renumbering(roster_t const & a, marking_map const & a_markings,
                        roster_t const & b, marking_map const & b_markings);


// various (circular?) dependencies prevent inclusion of restrictions.hh
class node_restriction;

void
make_restricted_roster(roster_t const & from, roster_t const & to,
                       roster_t & restricted,
                       node_restriction const & mask);

void
select_nodes_modified_by_cset(cset const & cs,
                              roster_t const & old_roster,
                              roster_t const & new_roster,
                              std::set<node_id> & nodes_modified);

void
get_content_paths(roster_t const & roster,
                  std::map<file_id, file_path> & paths);

// These functions are for the use of things like 'update' or 'pluck', that
// need to construct fake rosters and/or markings in-memory, to achieve
// particular merge results.
void
mark_roster_with_no_parents(revision_id const & rid,
                            roster_t const & roster,
                            marking_map & markings);
void
mark_roster_with_one_parent(roster_t const & parent,
                            marking_map const & parent_markings,
                            revision_id const & child_rid,
                            roster_t const & child,
                            marking_map & child_markings);

void
mark_merge_roster(roster_t const & left_roster,
                  marking_map const & left_markings,
                  std::set<revision_id> const & left_uncommon_ancestors,
                  roster_t const & right_roster,
                  marking_map const & right_markings,
                  std::set<revision_id> const & right_uncommon_ancestors,
                  revision_id const & new_rid,
                  roster_t const & merge,
                  marking_map & new_markings);

// These functions are an internal interface between ancestry.cc and
// roster.cc; unless you know exactly what you're doing you probably want
// something else.

void
make_roster_for_merge(revision_id const & left_rid,
                      roster_t const & left_roster,
                      marking_map const & left_markings,
                      cset const & left_cs,
                      std::set<revision_id> const & left_uncommon_ancestors,

                      revision_id const & right_rid,
                      roster_t const & right_roster,
                      marking_map const & right_markings,
                      cset const & right_cs,
                      std::set<revision_id> const & right_uncommon_ancestors,

                      revision_id const & new_rid,
                      roster_t & new_roster,
                      marking_map & new_markings,
                      node_id_source & nis);

void
make_roster_for_nonmerge(cset const & cs,
                         revision_id const & new_rid,
                         roster_t & new_roster, marking_map & new_markings,
                         node_id_source & nis);


// This is for revisions that are being written to the db, only.  It assigns
// permanent node ids.
void
make_roster_for_revision(database & db,
                         revision_t const & rev,
                         revision_id const & rid,
                         roster_t & result,
                         marking_map & marking);

// This is for revisions that are not necessarily going to be written to the
// db; you can specify your own node_id_source.
void
make_roster_for_revision(database & db,
                         node_id_source & nis,
                         revision_t const & rev,
                         revision_id const & rid,
                         roster_t & result,
                         marking_map & marking);

void
read_roster_and_marking(roster_data const & dat,
                        roster_t & ros,
                        marking_map & mm);

roster_data
write_roster_and_marking(roster_t const & ros,
                         marking_map const & mm);

manifest_data
write_manifest_of_roster(roster_t const & ros,
                         bool do_sanity_check = true);


manifest_id calculate_ident(roster_t const & ros,
                            bool do_sanity_check = true);

// for roster_delta

void append_with_escaped_quotes(std::string & collection,
                                std::string const & item);
void push_marking(std::string & contents,
                  bool is_file, const_marking_t const & mark,
                  int symbol_length);
void parse_marking(basic_io::parser & pa, marking_t & marking);

// Parent maps are used in a number of places to keep track of all the
// parent rosters of a given revision.

inline revision_id const & parent_id(parent_entry const & p)
{
  return p.first;
}

inline revision_id const & parent_id(parent_map::const_iterator i)
{
  return i->first;
}

inline cached_roster const &
parent_cached_roster(parent_entry const & p)
{
  return p.second;
}

inline cached_roster const &
parent_cached_roster(parent_map::const_iterator i)
{
  return i->second;
}

inline roster_t const & parent_roster(parent_entry const & p)
{
  return *(p.second.first);
}

inline roster_t const & parent_roster(parent_map::const_iterator i)
{
  return *(i->second.first);
}

inline marking_map const & parent_marking(parent_entry const & p)
{
  return *(p.second.second);
}

inline marking_map const & parent_marking(parent_map::const_iterator i)
{
  return *(i->second.second);
}

#endif

// Local Variables:
// mode: C++
// fill-column: 76
// c-file-style: "gnu"
// indent-tabs-mode: nil
// End:
// vim: et:sw=2:sts=2:ts=2:cino=>2s,{s,\:s,+s,t0,g0,^-2,e-2,n-2,p2s,(0,=s:
