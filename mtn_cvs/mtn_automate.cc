// -*- mode: C++; c-file-style: "gnu"; indent-tabs-mode: nil -*-
// copyright (C) 2006 Christof Petig <christof@petig-baender.de>
// all rights reserved.
// licensed to the public under the terms of the GNU GPL (>= 2)
// see the file COPYING for details

#include "mtn_automate.hh"
#include <sanity.hh>
#include <basic_io.hh>
#include <constants.hh>
#include <safe_map.hh>
#include <fstream>
#include <iostream>
#include <set>
#include <transforms.hh>

using std::string;
using std::make_pair;
using std::pair;

std::string const branch_cert_name_s("branch");
std::string const date_cert_name_s("date");
std::string const author_cert_name_s("author");
std::string const changelog_cert_name_s("changelog");
std::string const sync_cert_name_s("mtn-cvs-sync");

void mtn_automate::check_interface_revision(std::string const& minimum)
{ std::string present=automate("interface_version");
  N(present>=minimum,
      F("your monotone automate interface revision %s does not match the "
          "requirements %s") % present % minimum);
}

std::string mtn_automate::get_option(std::string const& name)
{
  return automate("get_option",std::vector<std::string>(1,name));
}

namespace
{
  namespace syms
  {
    // cset symbols
    symbol const delete_node("delete");
    symbol const rename_node("rename");
    symbol const content("content");
    symbol const add_file("add_file");
    symbol const add_dir("add_dir");
    symbol const patch("patch");
    symbol const from("from");
    symbol const to("to");
    symbol const clear("clear");
    symbol const set("set");
    symbol const attr("attr");
    symbol const value("value");
    
    // revision
    symbol const old_revision("old_revision");
    
    // roster symbols
    symbol const format_version("format_version");
//    symbol const old_revision("old_revision");
    symbol const new_manifest("new_manifest");
    
    symbol const dir("dir");
    symbol const file("file");
//    symbol const content("content");
//    symbol const attr("attr");

    // cmd_list symbols
    symbol const key("key");
    symbol const signature("signature");
    symbol const name("name");
//    symbol const value("value");
    symbol const trust("trust");
  }
}

static inline void
parse_path(basic_io::parser & parser, split_path & sp)
{
  std::string s;
  parser.str(s);
  file_path_internal(s).split(sp);
}

file_id mtn_automate::put_file(file_data const& d, file_id const& base)
{ std::vector<std::string> args;
  if (!null_id(base.inner())) args.push_back(base.inner()());
  args.push_back(d.inner()());
  return file_id(automate("put_file",args).substr(0,constants::idlen));
}

file_data mtn_automate::get_file(file_id const& fid)
{ std::vector<std::string> args;
  args.push_back(fid.inner()());
  return file_data(automate("get_file",args));
}

#include <piece_table.hh>

std::vector<revision_id> mtn_automate::get_revision_children(revision_id const& rid)
{ std::vector<std::string> args;
  args.push_back(rid.inner()());
  std::string children=automate("children",args);
  std::vector<revision_id> result;
  piece::piece_table lines;
  piece::index_deltatext(children,lines);
  result.reserve(children.size());
  for (piece::piece_table::const_iterator p=lines.begin();p!=lines.end();++p)
  { // L(FL("child '%s'") % (**p).substr(0,constants::idlen));
    result.push_back(revision_id((**p).substr(0,constants::idlen)));
  }
  piece::reset();
  return result;
}

std::vector<revision_id> mtn_automate::get_revision_parents(revision_id const& rid)
{ std::vector<std::string> args;
  args.push_back(rid.inner()());
  std::string children=automate("parents",args);
  std::vector<revision_id> result;
  piece::piece_table lines;
  piece::index_deltatext(children,lines);
  result.reserve(children.size());
  for (piece::piece_table::const_iterator p=lines.begin();p!=lines.end();++p)
    result.push_back(revision_id((**p).substr(0,constants::idlen)));
  piece::reset();
  return result;
}

std::vector<revision_id> mtn_automate::heads(std::string const& branch)
{ std::vector<std::string> args(1,branch);
  std::string heads=automate("heads",args);
  std::vector<revision_id> result;
  piece::piece_table lines;
  piece::index_deltatext(heads,lines);
  result.reserve(heads.size());
  for (piece::piece_table::const_iterator p=lines.begin();p!=lines.end();++p)
    result.push_back(revision_id((**p).substr(0,constants::idlen)));
  piece::reset();
  return result;
}

static void print_cset(basic_io::printer &printer, mtn_automate::cset const& cs)
{ for (path_set::const_iterator i = cs.nodes_deleted.begin();
       i != cs.nodes_deleted.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::delete_node, *i);
      printer.print_stanza(st);
    }

  for (std::map<split_path, split_path>::const_iterator i = cs.nodes_renamed.begin();
       i != cs.nodes_renamed.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::rename_node, file_path(i->first));
      st.push_file_pair(syms::to, file_path(i->second));
      printer.print_stanza(st);
    }

  for (path_set::const_iterator i = cs.dirs_added.begin();
       i != cs.dirs_added.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::add_dir, *i);
      printer.print_stanza(st);
    }

  for (std::map<split_path, file_id>::const_iterator i = cs.files_added.begin();
       i != cs.files_added.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::add_file, i->first);
      st.push_hex_pair(syms::content, i->second.inner());
      printer.print_stanza(st);
    }

  for (std::map<split_path, std::pair<file_id, file_id> >::const_iterator i = cs.deltas_applied.begin();
       i != cs.deltas_applied.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::patch, i->first);
      st.push_hex_pair(syms::from, i->second.first.inner());
      st.push_hex_pair(syms::to, i->second.second.inner());
      printer.print_stanza(st);
    }

  for (std::set<std::pair<split_path, attr_key> >::const_iterator i = cs.attrs_cleared.begin();
       i != cs.attrs_cleared.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::clear, file_path(i->first));
      st.push_str_pair(syms::attr, i->second());
      printer.print_stanza(st);
    }

  for (std::map<std::pair<split_path, attr_key>, attr_value>::const_iterator i = cs.attrs_set.begin();
       i != cs.attrs_set.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::set, file_path(i->first.first));
      st.push_str_pair(syms::attr, i->first.second());
      st.push_str_pair(syms::value, i->second());
      printer.print_stanza(st);
    }
}

revision_id mtn_automate::put_revision(revision_id const& parent, cset const& changes)
{ basic_io::printer printer;
  basic_io::stanza format_stanza;
  format_stanza.push_str_pair(syms::format_version, "1");
  printer.print_stanza(format_stanza);
      
  basic_io::stanza manifest_stanza;
  manifest_stanza.push_hex_pair(syms::new_manifest, hexenc<id>("0000000000000000000000000000000000000001"));
  printer.print_stanza(manifest_stanza);

// changeset stanza  
  basic_io::stanza st;
  st.push_hex_pair(syms::old_revision, parent.inner());
  printer.print_stanza(st);
  print_cset(printer, changes);
  std::vector<std::string> args(1,printer.buf);
  return revision_id(automate("put_revision",args).substr(0,constants::idlen));
}

mtn_automate::manifest_map mtn_automate::get_manifest_of(revision_id const& rid)
{ std::vector<std::string> args(1,rid.inner()());
  std::string aresult=automate("get_manifest_of",args);
  
  basic_io::input_source source(aresult,"automate get_manifest_of result");
  basic_io::tokenizer tokenizer(source);
  basic_io::parser pa(tokenizer);

  manifest_map result;
  // like roster_t::parse_from
  {
    pa.esym(syms::format_version);
    std::string vers;
    pa.str(vers);
    I(vers == "1");
  }

  while(pa.symp())
    {
      std::string pth, ident, rev;

      if (pa.symp(syms::file))
        {
          std::string content;
          pa.sym();
          pa.str(pth);
          pa.esym(syms::content);
          pa.hex(content);
          result[file_path_internal(pth)].first=file_id(content);
        }
      else if (pa.symp(syms::dir))
        {
          pa.sym();
          pa.str(pth);
          result[file_path_internal(pth)] /*=file_id()*/;
        }
      else
        break;
      
      // Non-dormant attrs
      while(pa.symp(basic_io::syms::attr))
        {
          pa.sym();
          std::string k, v;
          pa.str(k);
          pa.str(v);
          safe_insert(result[file_path_internal(pth)].second, 
                    make_pair(attr_key(k),attr_value(v)));
        }
        
      // Dormant attrs
      while(pa.symp(basic_io::syms::dormant_attr))
        {
          pa.sym();
          string k;
          pa.str(k);
          safe_insert(result[file_path_internal(pth)].second, 
                    make_pair(attr_key(k),attr_value()));
        }
    }
  return result;
}

void mtn_automate::cert_revision(revision_id const& rid, string const& name, std::string const& value)
{ std::vector<std::string> args;
  args.push_back(rid.inner()());
  args.push_back(name);
  args.push_back(value);
  automate("cert",args);
}

std::vector<mtn_automate::certificate> mtn_automate::get_revision_certs(revision_id const& rid)
{ std::vector<std::string> args;
  args.push_back(rid.inner()());
  std::string aresult=automate("certs",args);

  basic_io::input_source source(aresult,"automate get_revision_certs result");
  basic_io::tokenizer tokenizer(source);
  basic_io::parser pa(tokenizer);
  
  std::vector<certificate> result;
  
  while (pa.symp())
  { certificate cert;
  
    I(pa.symp(syms::key));
    pa.sym();
    pa.str(cert.key);
  
    I(pa.symp(syms::signature));
    pa.sym();
    std::string sign;
    pa.str(sign);
    if (sign=="ok") cert.signature=certificate::ok;
    else if (sign=="bad") cert.signature=certificate::bad;
    else cert.signature=certificate::unknown;

    I(pa.symp(syms::name));
    pa.sym();
    pa.str(cert.name);

    I(pa.symp(syms::value));
    pa.sym();
    pa.str(cert.value);

    I(pa.symp(syms::trust));
    pa.sym();
    std::string trust;
    pa.str(trust);
    cert.trusted= trust=="trusted";
    
    result.push_back(cert);
  }
  return result;
}

std::vector<mtn_automate::certificate> mtn_automate::get_revision_certs(revision_id const& rid, string const& name)
{ std::vector<mtn_automate::certificate> result=get_revision_certs(rid);
  for (std::vector<mtn_automate::certificate>::iterator i=result.begin();i!=result.end();)
  { if (i->name!=name) i=result.erase(i);
    else ++i;
  }
  return result;
}

static void
parse_cset(basic_io::parser & parser,
           mtn_automate::cset & cs)
{
//  cs.clear();
  string t1, t2;
  MM(t1);
  MM(t2);
  split_path p1, p2;
  MM(p1);
  MM(p2);

//  split_path prev_path;
//  MM(prev_path);
//  pair<split_path, attr_key> prev_pair;
//  MM(prev_pair.first);
//  MM(prev_pair.second);

  // we make use of the fact that a valid split_path is never empty
//  prev_path.clear();
  while (parser.symp(syms::delete_node))
    {
      parser.sym();
      parse_path(parser, p1);
//      I(prev_path.empty() || p1 > prev_path);
//      prev_path = p1;
      safe_insert(cs.nodes_deleted, p1);
    }

//  prev_path.clear();
  while (parser.symp(syms::rename_node))
    {
      parser.sym();
      parse_path(parser, p1);
//      I(prev_path.empty() || p1 > prev_path);
//      prev_path = p1;
      parser.esym(syms::to);
      parse_path(parser, p2);
      safe_insert(cs.nodes_renamed, make_pair(p1, p2));
    }

//  prev_path.clear();
  while (parser.symp(syms::add_dir))
    {
      parser.sym();
      parse_path(parser, p1);
//      I(prev_path.empty() || p1 > prev_path);
//      prev_path = p1;
      safe_insert(cs.dirs_added, p1);
    }

//  prev_path.clear();
  while (parser.symp(syms::add_file))
    {
      parser.sym();
      parse_path(parser, p1);
//      I(prev_path.empty() || p1 > prev_path);
//      prev_path = p1;
      parser.esym(syms::content);
      parser.hex(t1);
      safe_insert(cs.files_added, make_pair(p1, file_id(t1)));
    }

//  prev_path.clear();
  while (parser.symp(syms::patch))
    {
      parser.sym();
      parse_path(parser, p1);
//      I(prev_path.empty() || p1 > prev_path);
//      prev_path = p1;
      parser.esym(syms::from);
      parser.hex(t1);
      parser.esym(syms::to);
      parser.hex(t2);
      safe_insert(cs.deltas_applied,
                  make_pair(p1, make_pair(file_id(t1), file_id(t2))));
    }

//  prev_pair.first.clear();
  while (parser.symp(syms::clear))
    {
      parser.sym();
      parse_path(parser, p1);
      parser.esym(syms::attr);
      parser.str(t1);
      pair<split_path, attr_key> new_pair(p1, attr_key(t1));
//      I(prev_pair.first.empty() || new_pair > prev_pair);
//      prev_pair = new_pair;
      safe_insert(cs.attrs_cleared, new_pair);
    }

//  prev_pair.first.clear();
  while (parser.symp(syms::set))
    {
      parser.sym();
      parse_path(parser, p1);
      parser.esym(syms::attr);
      parser.str(t1);
      pair<split_path, attr_key> new_pair(p1, attr_key(t1));
//      I(prev_pair.first.empty() || new_pair > prev_pair);
//      prev_pair = new_pair;
      parser.esym(syms::value);
      parser.str(t2);
      safe_insert(cs.attrs_set, make_pair(new_pair, attr_value(t2)));
    }
}

static void
parse_edge(basic_io::parser & parser,
           mtn_automate::edge_map & es)
{
  boost::shared_ptr<mtn_automate::cset> cs(new mtn_automate::cset());
//  MM(cs);
  revision_id old_rev;
  string tmp;

  parser.esym(syms::old_revision);
  parser.hex(tmp);
  old_rev = revision_id(tmp);

  parse_cset(parser, *cs);

  es.insert(make_pair(old_rev, cs));
}

mtn_automate::revision_t mtn_automate::get_revision(revision_id const& rid)
{ std::vector<std::string> args;
  args.push_back(rid.inner()());
  std::string aresult=automate("get_revision",args);
  
  basic_io::input_source source(aresult,"automate get_revision result");
  basic_io::tokenizer tokenizer(source);
  basic_io::parser parser(tokenizer);

  revision_t result;

// that's from parse_revision
  string tmp;
  parser.esym(syms::format_version);
  parser.str(tmp);
  E(tmp == "1",
    F("encountered a revision with unknown format, version '%s'\n"
      "I only know how to understand the version '1' format\n"
      "a newer version of mtn_cvs is required to complete this operation")
    % tmp);
  parser.esym(syms::new_manifest);
  parser.hex(tmp);
//  rev.new_manifest = manifest_id(tmp);
  while (parser.symp(syms::old_revision))
    parse_edge(parser, result.edges);

  return result;
}

template <> void
dump(file_path const& fp, string & out)
{ out=fp.as_internal();
}

std::string mtn_automate::print_sync_info(mtn_automate::sync_map_t const& data)
{ basic_io::printer printer;
  for (mtn_automate::sync_map_t::const_iterator i = data.begin(); i != data.end(); ++i)
    {
      basic_io::stanza st;
      st.push_file_pair(syms::set, file_path(i->first.first));
      st.push_str_pair(syms::attr, i->first.second());
      st.push_str_pair(syms::value, i->second());
      printer.print_stanza(st);
    }
  return printer.buf;
}

std::string mtn_automate::print_cset_info(mtn_automate::cset const& data)
{ basic_io::printer printer;
	print_cset(printer, data);
	return printer.buf;
}

static bool begins_with(const std::string &s, const std::string &sub)
{ std::string::size_type len=sub.size();
  if (s.size()<len) return false;
  return !s.compare(0,len,sub);
}

bool mtn_automate::in_branch(revision_id const& rid, 
                      std::string const& branch)
{
  std::vector<certificate> branch_certs = get_revision_certs(rid, branch_cert_name_s);
  for (std::vector<certificate>::const_iterator cert=branch_certs.begin(); 
      cert!=branch_certs.end(); ++cert)
  {
    if (!cert->trusted || cert->signature!=mtn_automate::certificate::ok)
      continue;
    if (cert->value == branch)
      return true;
  }
  return false;
}

bool mtn_automate::is_synchronized(revision_id const& rid, 
                      std::string const& domain)
{
  // look for a certificate
  std::vector<certificate> certs;
  certs=get_revision_certs(rid,sync_cert_name_s);
  for (std::vector<certificate>::iterator i=certs.begin();i!=certs.end();)
  { if (!i->trusted || i->signature!=mtn_automate::certificate::ok)
      continue;
    if (i->value==domain)
      return true;
  }
  return false;
}

// Name: find_newest_sync
// Arguments:
//   sync-domain
//   branch (optional)
// Added in: 3.2
// Purpose:
//   Get the newest revision which has a sync certificate
//   (or changed sync attributes)
// Output format:
//   revision ID
// Error conditions:
//   a runtime exception is thrown if no synchronized revisions are found 
//   in this domain
revision_id mtn_automate::find_newest_sync(std::string const& domain, std::string const& branch)
{ /* search the revisions in the branch in reverse topologically sorted order for the first
     one that is synchronized.  We don't have to worry about ties for CVS as CVS syncs must be
     linearly chained.
   */

#warning fix find_newest_sync
		// can replace this with something like:
		// 'automate erase_ancestors(automate select c:mtn-cvs-sync=domain)'
		// if you get one rev, that's the solution
		// 0 or more than 1 rev == error

  std::vector<revision_id> heads;
  std::set<revision_id> nodes_checked;
  heads=mtn_automate::heads(branch);
  revision_id rid;

  // heads contains the nodes who have had all their decendents checked

  if (heads.empty())
    return revision_id();

  while (!heads.empty())
  {
    rid = *heads.begin();
    L(FL("find_newest_sync: testing node %s") % rid);
    // std::cerr << "find_newest_sync: testing node " << rid << std::endl;
    heads.erase(heads.begin());
    if (is_synchronized(rid,domain))
      return rid;
    nodes_checked.insert(rid);
    // add to heads all parents (i) whose children (j) have all been checked
    // each node will be added just after its last child is checked
    std::vector<revision_id> parents=get_revision_parents(rid);
    for (std::vector<revision_id>::const_iterator i=parents.begin(); 
          i!=parents.end(); ++i)
    {
      std::vector<revision_id> children = get_revision_children(*i);
      std::vector<revision_id>::const_iterator j;
      for (j=children.begin(); j!=children.end(); ++j)
      {
        if (nodes_checked.find(*j) != nodes_checked.end())
          continue;
        if (!in_branch(*j, branch))
          continue;

        break;
      }
      if (j == children.end())
        heads.push_back(*i);
    }
  }
  N(false, F("no synchronized revision found in branch %s for domain %s")
        % branch % domain);
}

mtn_automate::sync_map_t mtn_automate::get_sync_info(revision_id const& rid, string const& domain)
{
  /* sync information is coded in DOMAIN: prefixed attributes
   */
  sync_map_t result;

  I(is_synchronized(rid, domain));

  revision_t rev=get_revision(rid);
  L(FL("get_sync_info: checking revision attributes %s") % rid);
  manifest_map m=get_manifest_of(rid);
  std::string prefix=domain+":";
  for (manifest_map::const_iterator i = m.begin();
     i != m.end(); ++i)
  {
    file_id fid = i->second.first;
    bool found_rev(false), found_sha(false), am_dir(fid.inner()() == "");
    for (attr_map_t::const_iterator j = i->second.second.begin();
         j != i->second.second.end(); ++j)
    {
      if (begins_with(j->first(),prefix))
      { 
        if (j->first() == prefix + "revision")
          found_rev = true;
        else if (j->first() == prefix + "sha1")
        {
          found_sha = true;
          I(fid.inner()().substr(0,6) == j->second());
        }
        split_path sp;
        i->first.split(sp);
        result[std::make_pair(sp,j->first)]=j->second;
      }
    }
    I(am_dir || (found_rev && found_sha));
  }
  return result;
}