// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/stat.h>

#include "config.h"

#include "crush/CrushWrapper.h"
#include "crush/grammar.h"


#include <iostream>
#include <fstream>
#include <stack>
#include <functional>
#include <string>
#include <cassert>
#include <map>

#include <typeinfo>

using namespace std;

typedef char const*         iterator_t;                                                                              
typedef tree_match<iterator_t> parse_tree_match_t;                                                                   
typedef parse_tree_match_t::tree_iterator iter_t;
typedef parse_tree_match_t::node_t node_t;

int verbose = 0;

map<string, int> item_id;
map<int, string> id_item;
map<int, float> item_weight;

map<int, int> device_offload;  // may or may not be set.

map<string, int> type_id;

map<string, int> rule_id;

string string_node(node_t &node)
{
  return string(node.value.begin(), node.value.end());
}

int int_node(node_t &node) 
{
  string str = string_node(node);
  return strtol(str.c_str(), 0, 10);
}

float float_node(node_t &node)
{
  string s = string_node(node);
  return strtof(s.c_str(), 0);
}

void parse_device(iter_t const& i, CrushWrapper &crush)
{
  int size = i->children.size();
  int id = int_node(i->children[1]);

  string name = string_node(i->children[2]);
  crush.set_item_name(id, name.c_str());
  if (item_id.count(name)) {
    cerr << "item " << name << " defined twice" << std::endl;
    exit(1);
  }    
  item_id[name] = id;
  id_item[id] = name;

  if (verbose) cout << "device " << id << " " << name;

  if (size >= 4) {
    string tag = string_node(i->children[3]);
    float offload;
    if (tag == "offload") 
      offload = float_node(i->children[4]);
    else if (tag == "load") 
      offload = 1.0 - float_node(i->children[4]);
    else if (tag == "down") 
      offload = 1.0;
    else
      assert(0);

    if (verbose) cout << " offload " << offload;
    if (offload < 0 || offload > 1.0) {
      cerr << "illegal device offload " << offload << " on device " << id << " " << name
	   << " (valid range is [0,1])" << std::endl;
      exit(1);
    }
    device_offload[id] = (unsigned)(offload * 0x10000);
  }
  if (verbose) cout << std::endl;

  if (id >= crush.get_max_devices())
    crush.set_max_devices(id+1);
}

void parse_bucket_type(iter_t const& i, CrushWrapper &crush)
{
  int id = int_node(i->children[1]);
  string name = string_node(i->children[2]);
  if (verbose) cout << "type " << id << " " << name << std::endl;
  type_id[name] = id;
  crush.set_type_name(id, name.c_str());
}

void parse_bucket(iter_t const& i, CrushWrapper &crush)
{
  string tname = string_node(i->children[0]);
  if (!type_id.count(tname)) {
    cerr << "bucket type '" << tname << "' is not defined" << std::endl;
    exit(1);
  }
  int type = type_id[tname];

  string name = string_node(i->children[1]);
  if (item_id.count(name)) {
    cerr << "bucket or device '" << name << "' is already defined" << std::endl;
    exit(1);
  }

  int id = 0;  // none, yet!
  int alg = -1;
  set<int> used_items;
  int size = 0;
  
  for (unsigned p=3; p<i->children.size()-1; p++) {
    iter_t sub = i->children.begin() + p;
    string tag = string_node(sub->children[0]);
    //cout << "tag " << tag << std::endl;
    if (tag == "id") 
      id = int_node(sub->children[1]);
    else if (tag == "alg") {
      string a = string_node(sub->children[1]);
      if (a == "uniform")
	alg = CRUSH_BUCKET_UNIFORM;
      else if (a == "list")
	alg = CRUSH_BUCKET_LIST;
      else if (a == "tree")
	alg = CRUSH_BUCKET_TREE;
      else if (a == "straw")
	alg = CRUSH_BUCKET_STRAW;
      else {
	cerr << "unknown bucket alg '" << a << "'" << std::endl;
	exit(1);
      }
    }
    else if (tag == "item") {
      // first, just determine which item pos's are already used
      size++;
      for (unsigned q = 2; q < sub->children.size(); q++) {
	string tag = string_node(sub->children[q++]);
	if (tag == "pos") {
	  int pos = int_node(sub->children[q]);
	  if (used_items.count(pos)) {
	    cerr << "item '" << string_node(sub->children[1]) << "' in bucket '" << name << "' has explicit pos " << pos << ", which is occupied" << std::endl;
	    exit(1);
	  }
	  used_items.insert(pos);
	}
      }
    }
    else assert(0);
  }

  // now do the items.
  if (!used_items.empty())
    size = MAX(size, *used_items.rbegin());
  vector<int> items(size);
  vector<int> weights(size);

  int curpos = 0;
  float bucketweight = 0;
  for (unsigned p=3; p<i->children.size()-1; p++) {
    iter_t sub = i->children.begin() + p;
    string tag = string_node(sub->children[0]);
    if (tag == "item") {

      string iname = string_node(sub->children[1]);
      if (!item_id.count(iname)) {
	cerr << "item '" << iname << "' in bucket '" << name << "' is not defined" << std::endl;
	exit(1);
      }
      int itemid = item_id[iname];

      float weight = 1.0;
      if (item_weight.count(itemid))
	weight = item_weight[itemid];

      int pos = -1;
      for (unsigned q = 2; q < sub->children.size(); q++) {
	string tag = string_node(sub->children[q++]);
	if (tag == "weight")
	  weight = float_node(sub->children[q]);
	else if (tag == "pos") 
	  pos = int_node(sub->children[q]);
	else
	  assert(0);
      }
      if (pos >= size) {
	cerr << "item '" << iname << "' in bucket '" << name << "' has pos " << pos << " >= size " << size << std::endl;
	exit(1);
      }
      if (pos < 0) {
	while (used_items.count(curpos)) curpos++;
	pos = curpos++;
      }
      //cout << " item " << iname << " (" << itemid << ") pos " << pos << " weight " << weight << std::endl;
      items[pos] = itemid;
      weights[pos] = (unsigned)(weight * 0x10000);
      bucketweight += weight;
    }
  }

  if (id == 0) {
    for (id=-1; id_item.count(id); id--) ;
    //cout << "assigned id " << id << std::endl;
  }

  if (verbose) cout << "bucket " << name << " (" << id << ") " << size << " items and weight " << bucketweight << std::endl;
  id_item[id] = name;
  item_id[name] = id;
  item_weight[id] = bucketweight;
  
  crush.add_bucket(id, alg, type, size, &items[0], &weights[0]);
  crush.set_item_name(id, name.c_str());
}

void parse_rule(iter_t const& i, CrushWrapper &crush)
{
  int start;  // rule name is optional!
 
  string rname = string_node(i->children[1]);
  if (rname != "{") {
    if (rule_id.count(rname)) {
      cerr << "rule name '" << rname << "' already defined\n" << std::endl;
      exit(1);
    }
    start = 4;
  } else {
    rname = string();
    start = 3;
  }

  int pool = int_node(i->children[start]);

  string tname = string_node(i->children[start+2]);
  int type;
  if (tname == "replicated")
    type = CEPH_PG_TYPE_REP;
  else if (tname == "raid4") 
    type = CEPH_PG_TYPE_RAID4;
  else 
    assert(0);    

  int minsize = int_node(i->children[start+4]);
  int maxsize = int_node(i->children[start+6]);
  
  int steps = i->children.size() - start - 8;
  //cout << "num steps " << steps << std::endl;
  
  int ruleno = crush.add_rule(steps, pool, type, minsize, maxsize, -1);
  if (rname.length()) {
    crush.set_rule_name(ruleno, rname.c_str());
    rule_id[rname] = ruleno;
  }

  int step = 0;
  for (iter_t p = i->children.begin() + start + 7; step < steps; p++) {
    iter_t s = p->children.begin() + 1;
    int stepid = s->value.id().to_long();
    switch (stepid) {
    case crush_grammar::_step_take: 
      {
	string item = string_node(s->children[1]);
	if (!item_id.count(item)) {
	  cerr << "in rule '" << rname << "' item '" << item << "' not defined" << std::endl;
	  exit(1);
	}
	crush.set_rule_step_take(ruleno, step++, item_id[item]);
      }
      break;

    case crush_grammar::_step_choose:
    case crush_grammar::_step_chooseleaf:
      {
	string type = string_node(s->children[4]);
	if (!type_id.count(type)) {
	  cerr << "in rule '" << rname << "' type '" << type << "' not defined" << std::endl;
	  exit(1);
	}
	string choose = string_node(s->children[0]);
	string mode = string_node(s->children[1]);
	if (choose == "choose") {
	  if (mode == "firstn")
	    crush.set_rule_step_choose_firstn(ruleno, step++, int_node(s->children[2]), type_id[type]);
	  else if (mode == "indep")
	    crush.set_rule_step_choose_indep(ruleno, step++, int_node(s->children[2]), type_id[type]);
	  else assert(0);
	} else if (choose == "chooseleaf") {
	  if (mode == "firstn") 
	    crush.set_rule_step_choose_leaf_firstn(ruleno, step++, int_node(s->children[2]), type_id[type]);
	  else if (mode == "indep")
	    crush.set_rule_step_choose_leaf_indep(ruleno, step++, int_node(s->children[2]), type_id[type]);
	  else assert(0);
	} else assert(0);
      }
      break;

    case crush_grammar::_step_emit:
      crush.set_rule_step_emit(ruleno, step++);
      break;

    default:
      cerr << "bad crush step " << stepid << std::endl;
      assert(0);
    }
  }
  assert(step == steps);
}

void dump(iter_t const& i, int ind=1) 
{
  cout << "dump"; 
  for (int j=0; j<ind; j++) cout << "\t"; 
  long id = i->value.id().to_long();
  cout << id << "\t"; 
  cout << "'" << string(i->value.begin(), i->value.end())  
       << "' " << i->children.size() << " children" << std::endl; 
  for (unsigned int j = 0; j < i->children.size(); j++)  
    dump(i->children.begin() + j, ind+1); 
}

void find_used_bucket_ids(iter_t const& i)
{
  for (iter_t p = i->children.begin(); p != i->children.end(); p++) {
    if ((int)p->value.id().to_long() == crush_grammar::_bucket) {
      iter_t firstline = p->children.begin() + 3;
      string tag = string_node(firstline->children[0]);
      if (tag == "id") {
	int id = int_node(firstline->children[1]);
	//cout << "saw bucket id " << id << std::endl;
	id_item[id] = string();
      }
    }
  }
}

void parse_crush(iter_t const& i, CrushWrapper &crush) 
{ 
  find_used_bucket_ids(i);

  for (iter_t p = i->children.begin(); p != i->children.end(); p++) {
    switch (p->value.id().to_long()) {
    case crush_grammar::_device: 
      parse_device(p, crush);
      break;
    case crush_grammar::_bucket_type: 
      parse_bucket_type(p, crush);
      break;
    case crush_grammar::_bucket: 
      parse_bucket(p, crush);
      break;
    case crush_grammar::_crushrule: 
      parse_rule(p, crush);
      break;
    default:
      assert(0);
    }
  }

  //cout << "max_devices " << crush.get_max_devices() << std::endl;
  crush.finalize();
  for (int i=0; i<crush.get_max_devices(); i++)
    if (device_offload.count(i))
      crush.set_offload(i, device_offload[i]);
  
} 

const char *infn = "stdin";


////////////////////////////////////////////////////////////////////////////


int compile_crush_file(const char *infn, CrushWrapper &crush)
{ 
  // read the file
  ifstream in(infn);
  if (!in.is_open()) {
    cerr << "input file " << infn << " not found" << std::endl;
    return -ENOENT;
  }

  string big;
  string str;
  int line = 1;
  map<int,int> line_pos;  // pos -> line
  map<int,string> line_val;
  while (getline(in, str)) {
    // remove newline
    int l = str.length();
    if (l && str[l] == '\n')
      str.erase(l-1, 1);

    line_val[line] = str;

    // strip comment
    int n = str.find("#");
    if (n >= 0)
      str.erase(n, str.length()-n);
    
    if (verbose>1) cout << line << ": " << str << std::endl;

    if (big.length()) big += " ";
    line_pos[big.length()] = line;
    line++;
    big += str;
  }
  
  if (verbose > 2) cout << "whole file is: \"" << big << "\"" << std::endl;
  
  crush_grammar crushg;
  const char *start = big.c_str();
  //tree_parse_info<const char *> info = ast_parse(start, crushg, space_p);
  tree_parse_info<> info = ast_parse(start, crushg, space_p);
  
  // parse error?
  if (!info.full) {
    int cpos = info.stop - start;
    //cout << "cpos " << cpos << std::endl;
    //cout << " linemap " << line_pos << std::endl;
    assert(!line_pos.empty());
    map<int,int>::iterator p = line_pos.upper_bound(cpos);
    if (p != line_pos.begin()) p--;
    int line = p->second;
    int pos = cpos - p->first;
    cerr << infn << ":" << line //<< ":" << (pos+1)
	 << " error: parse error at '" << line_val[line].substr(pos) << "'" << std::endl;
    return 1;
  }

  //cout << "parsing succeeded\n";
  //dump(info.trees.begin());
  parse_crush(info.trees.begin(), crush);
  
  return 0;
}

void print_type_name(ostream& out, int t, CrushWrapper &crush)
{
  const char *name = crush.get_type_name(t);
  if (name)
    out << name;
  else if (t == 0)
    out << "device";
  else
    out << "type" << t;
}

void print_item_name(ostream& out, int t, CrushWrapper &crush)
{
  const char *name = crush.get_item_name(t);
  if (name)
    out << name;
  else if (t >= 0)
    out << "device" << t;
  else
    out << "bucket" << (-1-t);
}

void print_rule_name(ostream& out, int t, CrushWrapper &crush)
{
  const char *name = crush.get_rule_name(t);
  if (name)
    out << name;
  else
    out << "rule" << t;
}

void print_fixedpoint(ostream& out, int i)
{
  char s[20];
  sprintf(s, "%.3f", (float)i / (float)0x10000);
  out << s;
}

int decompile_crush(CrushWrapper &crush, ostream &out)
{
  out << "# begin crush map\n\n";

  out << "# devices\n";
  for (int i=0; i<crush.get_max_devices(); i++) {
    //if (crush.get_item_name(i) == 0)
    //continue;
    out << "device " << i << " ";
    print_item_name(out, i, crush);
    if (crush.get_device_offload(i)) {
      out << " offload ";
      print_fixedpoint(out, crush.get_device_offload(i));
    }
    out << "\n";
  }
  
  out << "\n# types\n";
  int n = crush.get_num_type_names();
  for (int i=0; n; i++) {
    const char *name = crush.get_type_name(i);
    if (!name) {
      if (i == 0) out << "type 0 device\n";
      continue;
    }
    n--;
    out << "type " << i << " " << name << "\n";
  }

  out << "\n# buckets\n";
  for (int i=-1; i > -1-crush.get_max_buckets(); i--) {
    if (!crush.bucket_exists(i)) continue;
    int type = crush.get_bucket_type(i);
    print_type_name(out, type, crush);
    out << " ";
    print_item_name(out, i, crush);
    out << " {\n";
    out << "\tid " << i << "\t\t# do not change unnecessarily\n";

    int n = crush.get_bucket_size(i);

    int alg = crush.get_bucket_alg(i);
    out << "\talg " << crush_bucket_alg_name(alg);

    // notate based on alg type
    bool dopos = false;
    switch (alg) {
    case CRUSH_BUCKET_UNIFORM:
      out << "\t# do not change bucket size (" << n << ") unnecessarily";
      dopos = true;
      break;
    case CRUSH_BUCKET_LIST:
      out << "\t# add new items at the end; do not change order unnecessarily";
      break;
    case CRUSH_BUCKET_TREE:
      out << "\t# do not change pos for existing items unnecessarily";
      dopos = true;
      break;
    }
    out << "\n";

    for (int j=0; j<n; j++) {
      int item = crush.get_bucket_item(i, j);
      int w = crush.get_bucket_item_weight(i, j);
      if (!w) {
	dopos = true;
	continue;
      }
      out << "\titem ";
      print_item_name(out, item, crush);
      out << " weight ";
      print_fixedpoint(out, w);
      if (dopos) {
	if (alg == CRUSH_BUCKET_TREE)
	  out << " pos " << (j-1)/2;
	else 
	  out << " pos " << j;
      }
      out << "\n";
    }
    out << "}\n";
  }

  out << "\n# rules\n";
  for (int i=0; i<crush.get_max_rules(); i++) {
    if (!crush.rule_exists(i)) continue;
    out << "rule ";
    if (crush.get_rule_name(i))
      print_rule_name(out, i, crush);
    out << " {\n";
    out << "\tpool " << crush.get_rule_mask_pool(i) << "\n";
    switch (crush.get_rule_mask_type(i)) {
    case CEPH_PG_TYPE_REP: out << "\ttype replicated\n"; break;
    case CEPH_PG_TYPE_RAID4: out << "\ttype raid4\n"; break;
    default: out << "\ttype " << crush.get_rule_mask_type(i) << "\n";
    }
    out << "\tmin_size " << crush.get_rule_mask_min_size(i) << "\n";
    out << "\tmax_size " << crush.get_rule_mask_max_size(i) << "\n";
    for (int j=0; j<crush.get_rule_len(i); j++) {
      switch (crush.get_rule_op(i, j)) {
      case CRUSH_RULE_NOOP:
	out << "\tstep noop\n";
	break;
      case CRUSH_RULE_TAKE:
	out << "\tstep take ";
	print_item_name(out, crush.get_rule_arg1(i, j), crush);
	out << "\n";
	break;
      case CRUSH_RULE_EMIT:
	out << "\tstep emit\n";
	break;
      case CRUSH_RULE_CHOOSE_FIRSTN:
	out << "\tstep choose firstn "
	    << crush.get_rule_arg1(i, j) 
	    << " type ";
	print_type_name(out, crush.get_rule_arg2(i, j), crush);
	out << "\n";
	break;
      case CRUSH_RULE_CHOOSE_INDEP:
	out << "\tstep choose indep "
	    << crush.get_rule_arg1(i, j) 
	    << " type ";
	print_type_name(out, crush.get_rule_arg2(i, j), crush);
	out << "\n";
	break;
      case CRUSH_RULE_CHOOSE_LEAF_FIRSTN:
	out << "\tstep chooseleaf firstn "
	    << crush.get_rule_arg1(i, j) 
	    << " type ";
	print_type_name(out, crush.get_rule_arg2(i, j), crush);
	out << "\n";
	break;
      case CRUSH_RULE_CHOOSE_LEAF_INDEP:
	out << "\tstep chooseleaf indep "
	    << crush.get_rule_arg1(i, j) 
	    << " type ";
	print_type_name(out, crush.get_rule_arg2(i, j), crush);
	out << "\n";
	break;
      }
    }
    out << "}\n";
  }
  out << "\n# end crush map" << std::endl;
  return 0;
}


int usage(const char *me)
{
  cout << me << ": usage: crushtool [-d map] [-c map.txt] [-o outfile [--clobber]]" << std::endl;
  exit(1);
}

int main(int argc, const char **argv)
{

  vector<const char*> args;
  argv_to_vec(argc, argv, args);

  const char *me = argv[0];
  const char *cinfn = 0;
  const char *dinfn = 0;
  const char *outfn = 0;
  bool clobber = false;

  for (unsigned i=0; i<args.size(); i++) {
    if (strcmp(args[i], "--clobber") == 0) 
      clobber = true;
    else if (strcmp(args[i], "-d") == 0)
      dinfn = args[++i];
    else if (strcmp(args[i], "-o") == 0)
      outfn = args[++i];
    else if (strcmp(args[i], "-c") == 0)
      cinfn = args[++i];
    else if (strcmp(args[i], "-v") == 0)
      verbose++;
    else 
      usage(me);
  }
  if (cinfn && dinfn)
    usage(me);
  if (!cinfn && !dinfn)
    usage(me);

  /*
  if (outfn) cout << "outfn " << outfn << std::endl;
  if (cinfn) cout << "cinfn " << cinfn << std::endl;
  if (dinfn) cout << "dinfn " << dinfn << std::endl;
  */

  CrushWrapper crush;

  if (dinfn) {
    bufferlist bl;
    int r = bl.read_file(dinfn);
    if (r < 0) {
      cerr << me << ": error reading '" << dinfn << "': " << strerror(-r) << std::endl;
      exit(1);
    }
    bufferlist::iterator p = bl.begin();
    crush.decode(p);

    if (outfn) {
      ofstream o;
      o.open(outfn, ios::out | ios::binary | ios::trunc);
      if (!o.is_open()) {
	cerr << me << ": error writing '" << outfn << "'" << std::endl;
	exit(1);
      }
      decompile_crush(crush, o);
      o.close();
    } else 
      decompile_crush(crush, cout);
  }

  if (cinfn) {
    crush.create();
    int r = compile_crush_file(cinfn, crush);
    crush.finalize();
    if (r < 0) 
      exit(1);

    if (outfn) {
      bufferlist bl;
      crush.encode(bl);
      int r = bl.write_file(outfn);
      if (r < 0) {
	cerr << me << ": error writing '" << outfn << "': " << strerror(-r) << std::endl;
	exit(1);
      }
      if (verbose)
	cout << "wrote crush map to " << outfn << std::endl;
    } else {
      cout << me << " successfully compiled '" << cinfn << "'.  Use -o file to write it out." << std::endl;
    }
  }

  return 0;
}
