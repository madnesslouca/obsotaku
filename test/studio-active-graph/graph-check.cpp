#include <obs.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <mutex>
extern "C" long studio_graph_show(obs_source_t *);
extern "C" long studio_graph_active(obs_source_t *);
extern "C" void studio_graph_tick(obs_source_t *);
extern "C" void studio_graph_drain(void);
extern "C" bool studio_graph_transfer_pair(obs_source_t*,obs_source_t*,obs_source_t*,obs_source_t*,obs_source_t*,obs_source_t*);
struct Node {obs_source_t *self;std::vector<obs_source_t *> visual;std::atomic<int> shown{0},hidden{0},activated{0},deactivated{0};obs_source_t *reenter=nullptr;};
static std::atomic<int> destroyed{0},enumCalls{0};static int failures=0;static obs_data_array_t *checks;
void check(bool pass,const std::string &name){auto *d=obs_data_create();obs_data_set_string(d,"check",name.c_str());obs_data_set_bool(d,"passed",pass);obs_data_array_push_back(checks,d);obs_data_release(d);if(!pass)++failures;std::cout<<(pass?"PASS ":"FAIL ")<<name<<std::endl;}
void count(obs_source_t *s,long show,long active,const std::string &label){check(studio_graph_show(s)==show&&studio_graph_active(s)==active,label);}
const char *name(void*){return "Private active-child graph test";}
void *create(obs_data_t *,obs_source_t *s){return new Node{s};}
void destroy(void *v){++destroyed;delete static_cast<Node*>(v);}
void enumerate(void *v,obs_source_enum_proc_t cb,void *p){auto *n=static_cast<Node*>(v);++enumCalls;for(auto *s:n->visual)cb(n->self,s,p);}
void show(void *v){auto *n=static_cast<Node*>(v);++n->shown;if(n->reenter){obs_source_inc_showing(n->reenter);obs_source_dec_showing(n->reenter);}}
void hide(void *v){++static_cast<Node*>(v)->hidden;}
void activate(void *v){++static_cast<Node*>(v)->activated;}
void deactivate(void *v){++static_cast<Node*>(v)->deactivated;}
Node *node(obs_source_t *s){return static_cast<Node*>(obs_obj_get_data(s));}
obs_source_t *source(const char *s){return obs_source_create_private("graph_probe",s,nullptr);}
int main(int argc,char **argv){if(argc!=2||std::filesystem::exists(argv[1]))return 2;checks=obs_data_array_create();check(obs_startup("en-US",nullptr,nullptr),"Real libobs startup");obs_source_info i{};i.id="graph_probe";i.type=OBS_SOURCE_TYPE_INPUT;i.output_flags=OBS_SOURCE_AUDIO;i.get_name=name;i.create=create;i.destroy=destroy;i.enum_active_sources=enumerate;i.enum_all_sources=enumerate;i.show=show;i.hide=hide;i.activate=activate;i.deactivate=deactivate;obs_register_source(&i);
 auto *r=source("root"),*a=source("A"),*b=source("B"),*c=source("C");
 check(obs_source_add_active_child(r,a)&&obs_source_add_active_child(r,b)&&obs_source_add_active_child(a,c)&&obs_source_add_active_child(b,c),"Diamond attach while inactive");
 int enumsBefore=enumCalls.load();obs_source_inc_active(r);check(enumCalls.load()==enumsBefore,"Root propagation does not invoke plugin enumeration");count(r,1,1,"Root owns one MAIN credit");count(a,1,1,"A inherited MAIN");count(c,2,2,"Diamond has two independent path credits");
 obs_source_dec_showing(r);count(c,2,2,"Wrong AUX decrement cannot consume MAIN showing credit");obs_source_inc_showing(r);count(c,4,2,"AUX propagation preserves separate active count");
 check(obs_source_add_active_child(a,c),"Duplicate edge accepted as independent membership");count(c,6,3,"Duplicate edge adds parent credit exactly once");
 obs_source_remove_active_child(a,c);count(c,4,2,"Removing one duplicate leaves one");obs_source_dec_active(r);count(c,2,0,"Removing own MAIN leaves only AUX");obs_source_dec_showing(r);count(c,0,0,"All root credits returned");
 obs_source_remove_active_child(r,a);obs_source_remove_active_child(r,b);obs_source_remove_active_child(a,c);obs_source_remove_active_child(b,c);
 obs_source_remove_active_child(a,c);count(c,0,0,"Unmatched remove cannot borrow credits");
 // Every mandatory API insertion is authoritative even before its visual attach.
 check(obs_source_add_active_child(r,a),"Create-before-visual-attach edge accepted");obs_source_inc_active(r);count(a,1,1,"Root activation observes edge before visual attachment");node(r)->visual={a};int seen=0;obs_source_enum_active_sources(r,[](obs_source_t*,obs_source_t*,void *v){++*static_cast<int*>(v);},&seen);check(seen==1,"Public enumeration still invokes plugin visual callback");node(r)->visual.clear();obs_source_remove_active_child(r,a);count(a,0,0,"Detach after physical visual removal returns exact credits");obs_source_dec_active(r);
 // Inactive full-tree cycle contract remains stronger than active ledger.
 node(a)->visual={r};check(!obs_source_add_active_child(r,a),"Inactive full-tree descendant cycle refused");node(a)->visual.clear();check(!obs_source_add_active_child(r,r),"Self edge refused");
 // Two concurrent insertions can pass generic enumeration but never publish a cycle.
 bool races=true;for(int k=0;k<120;k++){std::atomic<int> ready{0};std::atomic<bool> go{false};bool ab=false,ba=false;std::thread t1([&]{++ready;while(!go.load())std::this_thread::yield();ab=obs_source_add_active_child(a,b);});std::thread t2([&]{++ready;while(!go.load())std::this_thread::yield();ba=obs_source_add_active_child(b,a);});while(ready.load()!=2)std::this_thread::yield();go=true;t1.join();t2.join();races &= (ab!=ba);if(ab)obs_source_remove_active_child(a,b);if(ba)obs_source_remove_active_child(b,a);races &= studio_graph_show(a)==0&&studio_graph_show(b)==0;}check(races,"120 concurrent opposite-edge insertions reject exactly one cycle and clean up");
 // Attach/detach races with own activation: independent oracle after join is zero.
 std::atomic<bool> ghosts{true};for(int k=0;k<240;k++){std::atomic<bool> go{false};std::thread t1([&]{while(!go.load())std::this_thread::yield();for(int j=0;j<12;j++){obs_source_inc_active(r);obs_source_dec_active(r);}});std::thread t2([&]{while(!go.load())std::this_thread::yield();for(int j=0;j<12;j++){if(!obs_source_add_active_child(r,a))ghosts=false;obs_source_remove_active_child(r,a);}});go=true;t1.join();t2.join();if(!(studio_graph_show(r)==0&&studio_graph_active(r)==0&&studio_graph_show(a)==0&&studio_graph_active(a)==0))ghosts=false;}check(ghosts.load(),"2880 root activate/deactivate versus edge attach/detach cycles leave no ghost");
 // Reparent a live source, then reverse it. Both parents remain independently live.
 obs_source_inc_active(r);obs_source_inc_showing(b);check(obs_source_add_active_child(r,c),"Live reparent initial edge");count(c,1,1,"Live source inherits MAIN");obs_source_remove_active_child(r,c);check(obs_source_add_active_child(b,c),"Live reparent new edge");count(c,1,0,"Reparent inherits new AUX only");obs_source_remove_active_child(b,c);obs_source_dec_showing(b);obs_source_dec_active(r);
 // Tick delivery is legacy boolean reconciliation, never inline with graph updates.
 auto *n=node(a);n->reenter=c;obs_source_inc_active(a);check(n->shown==0&&n->activated==0,"No plugin activation delivery under graph operation");studio_graph_tick(a);check(n->shown==1&&n->activated==1,"Tick delivers show/activate with reentrant graph operation outside gate");count(c,0,0,"Reentrant show callback returned child root credits");obs_source_dec_active(a);studio_graph_tick(a);check(n->hidden==1&&n->deactivated==1,"Tick delivers matching hide/deactivate");obs_source_inc_active(a);obs_source_dec_active(a);studio_graph_tick(a);check(n->shown==1&&n->hidden==1,"Intermediate between-tick changes remain coalesced per legacy contract");n->reenter=nullptr;
 // A rejected later move rolls back prior valid moves and original membership.
 check(obs_source_add_active_child(r,a)&&obs_source_add_active_child(a,c)&&obs_source_add_active_child(b,c),"Batch rollback setup DAG");obs_source_inc_active(r);obs_source_inc_showing(b);count(c,2,1,"Batch rollback initial path credits");check(!studio_graph_transfer_pair(b,r,c,r,c,a),"Batch rejects cycle after an earlier valid move");count(c,2,1,"Rejected batch restores exact inherited credits");obs_source_remove_active_child(b,c);count(c,1,1,"Rejected batch restores original edge identity and ownership");obs_source_remove_active_child(a,c);obs_source_remove_active_child(r,a);obs_source_dec_active(r);obs_source_dec_showing(b);
 // Own root credit is independent of inherited credit.
 check(obs_source_add_active_child(r,a),"Own/inherited separation edge");obs_source_inc_active(r);obs_source_dec_active(a);count(a,1,1,"Unmatched child root decrement cannot consume inherited credit");obs_source_remove_active_child(r,a);obs_source_dec_active(r);count(a,0,0,"Parent detach removes final inherited credit");
 for(auto *s:{r,a,b,c})obs_source_release(s);studio_graph_drain();check(destroyed==4,"All four graph nodes destroyed after balanced graph release");
 // Fallback handles a plugin that fails to detach at destruction without freeing under gate.
 auto *p=source("orphan parent"),*q=source("orphan child");check(obs_source_add_active_child(p,q),"Private lifetime edge owns strong child reference");obs_source_release(q);studio_graph_drain();check(destroyed==4,"Child remains alive while parent edge owns it");obs_source_release(p);studio_graph_drain();check(destroyed==6,"Parent fallback drain releases orphan edge and final child outside gate");
 obs_shutdown();auto *d=obs_data_create();obs_data_set_int(d,"failures",failures);obs_data_set_array(d,"checks",checks);obs_data_set_bool(d,"passed",!failures);obs_data_set_int(d,"native_opposite_edge_races",120);obs_data_set_int(d,"native_root_edge_race_cycles",2880);obs_data_save_json_safe(d,argv[1],"tmp",nullptr);obs_data_release(d);obs_data_array_release(checks);return failures?1:0;}
