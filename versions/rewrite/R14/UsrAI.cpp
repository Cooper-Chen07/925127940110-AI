#include "UsrAI.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <vector>

tagGame tagUsrGame;
ins UsrIns;

namespace newaoe_ai {
struct Pos {
    double x = 0, y = 0;
    Pos() {}
    Pos(double a, double b) : x(a), y(b) {}
    Pos operator+(Pos p) const { return Pos(x+p.x,y+p.y); }
    Pos operator-(Pos p) const { return Pos(x-p.x,y-p.y); }
    Pos operator*(double k) const { return Pos(x*k,y*k); }
};
double length(Pos p) { return std::sqrt(p.x*p.x+p.y*p.y); }
double distance(Pos a, Pos b) { return length(a-b); }
Pos unit(Pos p) { double n=length(p); return n>0.001 ? p*(1/n) : Pos(1,0); }
Pos position(const tagHuman& h) { const double scale=static_cast<double>(BLOCKSIDELENGTH); return Pos(h.DR/scale,h.UR/scale); }
Pos position(const tagResource& r) { const double scale=static_cast<double>(BLOCKSIDELENGTH); return Pos(r.DR/scale,r.UR/scale); }
int sizeOf(int t) { return t==BUILDING_HOME||t==BUILDING_ARROWTOWER ? 2:3; }
Pos position(const tagBuilding& b) { double s=sizeOf(b.Type)*0.5; return Pos(b.BlockDR+s,b.BlockUR+s); }
bool ranged(int t) { return t==AT_BOWMAN||t==AT_IMPROVED||t==AT_COMPOSITE_BOWMAN||t==AT_CHARIOT_ARCHER||t==AT_SLINGER||t==AT_STONE_THROWER; }
struct Order { int frame=-10000, kind=0, target=-1, id=-1; Pos point; };
struct Worker { int role=0, progress=0, cargo=-1, target=-1, blocked=-1, avoidUntil=0; Pos point; };
struct Tech { int state=0, building=-1, issued=0; bool observed=false; };

class Strategy {
    const tagInfo* s=nullptr;
    AI* api=nullptr;
    int frame=0, last=-1, logFrame=-1000, builder=-1, scout=-1;
    int phase=0, phaseFrame=0, lastThreat=0, advanceFrame=0;
    int priestRetreatUntil=0;
    Pos priestRetreat;
    int wood=0, food=0, gold=0, freePop=0;
    Pos base, front, direction;
    std::map<int,Order> orders;
    std::map<int,Worker> workers;
    std::map<std::pair<int,int>,int> blockedResources;
    std::map<std::pair<int,int>,int> blockedSites;
    std::map<int,Tech> techs;
    std::map<int,int> useCount;
    std::map<int,int> dodgeUntil, dodgeFrame;
    std::set<int> recovering;
    std::set<int> priestGuard;
    std::vector<Pos> marchRoute;
    std::vector<Pos> plannedRoute;
    int planFrame=-1000;
    std::map<int,tagBuilding> knownBuildings;
    std::set<int> sent;
    std::vector<std::vector<int>> occupied, reachable;
    std::vector<std::vector<tagTerrain>> terrain;
    int width=0, height=0;
    int pendingBuild=-1, buildFrame=0, buildX=0, buildY=0;
    int buildSequence=0, errors=0, rebalanceFrame=-1000;
    const tagArmy* priest=nullptr;
    const tagBuilding* center=nullptr;
    std::ofstream trace;

    const tagBuilding* building(int sn) const {
        for(const auto& b:s->buildings) if(b.SN==sn) return &b;
        return nullptr;
    }
    const tagArmy* enemy(int sn) const {
        for(const auto& a:s->enemy_armies) if(a.SN==sn&&a.Blood>0) return &a;
        return nullptr;
    }
    const tagArmy* army(int sn) const {
        for(const auto& a:s->armies) if(a.SN==sn&&a.Blood>0) return &a;
        return nullptr;
    }
    const tagFarmer* farmer(int sn) const {
        for(const auto& a:s->farmers) if(a.SN==sn&&a.Blood>0) return &a;
        return nullptr;
    }
    int count(int type, bool finished=false) const {
        int n=0; for(const auto& b:s->buildings) if(b.Type==type&&(!finished||b.Percent>=100)) ++n; return n;
    }
    int troops(bool bows, bool melee) const {
        int n=0; for(const auto& a:s->armies) if(a.Sort!=AT_PRIEST&&a.Blood>0)
            if((bows&&a.Sort==AT_COMPOSITE_BOWMAN)||(melee&&(bows||!ranged(a.Sort)))) ++n;
        return n;
    }
    bool done(int action) const { auto it=techs.find(action); return it!=techs.end()&&it->second.state==2; }
    bool inBounds(int x,int y) const { return x>=0&&y>=0&&x<width&&y<height; }
    bool land(int x,int y) const { return inBounds(x,y)&&terrain[x][y].type==MAPPATTERN_GRASS; }
    bool open(int x,int y) const { return land(x,y)&&!occupied[x][y]; }
    bool danger(Pos p, double radius) const {
        for(const auto& e:s->enemy_armies) if(e.Blood>0&&distance(p,position(e))<radius) return true;
        for(const auto& kv:knownBuildings) if(kv.second.Type==BUILDING_ARROWTOWER&&distance(p,position(kv.second))<11) return true;
        return false;
    }
    bool available(int sn,int gap=12) const {
        if(sent.count(sn)) return false;
        auto it=orders.find(sn); return it==orders.end()||frame-it->second.frame>=gap;
    }
    void remember(int sn,int kind,int target,Pos p,int id) {
        Order& o=orders[sn]; o.frame=frame; o.kind=kind; o.target=target; o.point=p; o.id=id; sent.insert(sn);
    }
    bool action(int sn,int target,int current,int state,int gap=18) {
        if(sn==target||target<0||!available(sn,gap)) return false;
        if(current==target&&state!=HUMAN_STATE_IDLE) return false;
        const auto it=orders.find(sn);
        if(it!=orders.end()&&it->second.kind==2&&it->second.target==target&&frame-it->second.frame<45) return false;
        remember(sn,2,target,Pos(),api->HumanAction(sn,target)); return true;
    }
    Pos freePoint(Pos goal,int ignore=-1,bool avoidUnits=true) const {
        Pos best=goal; double score=1e20;
        for(int dx=-4;dx<=4;++dx) for(int dy=-4;dy<=4;++dy) {
            int x=int(goal.x)+dx,y=int(goal.y)+dy; if(!open(x,y)) continue;
            Pos p(x+0.5,y+0.5); double v=distance(p,goal);
            if(avoidUnits){
                for(const auto& a:s->armies) if(a.SN!=ignore&&distance(position(a),p)<0.85) v+=1.5;
                for(const auto& a:s->farmers) if(a.SN!=ignore&&distance(position(a),p)<0.7) v+=1.5;
            }
            if(v<score){score=v; best=p;}
        }
        return best;
    }
    std::vector<Pos> route(Pos from,Pos to) const {
        std::vector<Pos> result;
        if(!width||!height)return result;
        int sx=std::max(0,std::min(width-1,int(from.x))),sy=std::max(0,std::min(height-1,int(from.y)));
        int tx=std::max(0,std::min(width-1,int(to.x))),ty=std::max(0,std::min(height-1,int(to.y)));
        const int cells=width*height,start=sx*height+sy,finish=tx*height+ty;
        std::vector<double> costs(cells,1e20);std::vector<int> parent(cells,-1);
        using Entry=std::pair<double,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
        auto heuristic=[&](int x,int y){return double(std::abs(x-tx)+std::abs(y-ty));};
        costs[start]=0;queue.push({heuristic(sx,sy),start});
        int best=start;double bestDistance=heuristic(sx,sy);
        const int dx[]={1,-1,0,0},dy[]={0,0,1,-1};
        while(!queue.empty()){
            auto item=queue.top();queue.pop();int id=item.second,x=id/height,y=id%height;
            if(item.first>costs[id]+heuristic(x,y)+0.01)continue;
            double remaining=heuristic(x,y);if(remaining<bestDistance){bestDistance=remaining;best=id;}
            if(id==finish){best=id;break;}
            for(int k=0;k<4;++k){int nx=x+dx[k],ny=y+dy[k];
                if(!inBounds(nx,ny)||occupied[nx][ny])continue;
                int type=terrain[nx][ny].type;if(type!=0&&type!=MAPPATTERN_GRASS)continue;
                if(type!=0&&terrain[x][y].type!=0&&terrain[x][y].height>=0&&terrain[nx][ny].height>=0&&terrain[x][y].height!=terrain[nx][ny].height)continue;
                int next=nx*height+ny;double nextCost=costs[id]+(type==0?2.5:1.0);
                if(nextCost<costs[next]){costs[next]=nextCost;parent[next]=id;queue.push({nextCost+heuristic(nx,ny),next});}
            }
        }
        for(int id=best;id!=start&&id>=0;id=parent[id])result.push_back(Pos(id/height+0.5,id%height+0.5));
        std::reverse(result.begin(),result.end());return result;
    }
    Pos priestAnchor() const {
        if(phase==0)return front-direction*2;
        if(marchRoute.empty())return front-direction*6;
        double behind=0;Pos previous=front;
        for(auto it=marchRoute.rbegin();it!=marchRoute.rend();++it){
            double segment=distance(previous,*it);
            if(behind+segment>=6&&segment>0.01)return previous+(*it-previous)*((6-behind)/segment);
            behind+=segment;previous=*it;
        }
        return marchRoute.front();
    }
    bool move(const tagHuman& h,Pos goal,int gap=30) {
        if(!available(h.SN,gap)) return false;
        goal=freePoint(goal,h.SN,!(priest&&h.SN==priest->SN));
        goal.x=std::max(0.5,std::min(width-0.5,goal.x));
        goal.y=std::max(0.5,std::min(height-0.5,goal.y));
        if(distance(position(h),goal)<0.65) return false;
        auto it=orders.find(h.SN);
        if(it!=orders.end()&&it->second.kind==1&&distance(it->second.point,goal)<1.1&&
           h.NowState==HUMAN_STATE_WALKING&&frame-it->second.frame<175) return false;
        const int sn=h.SN;
        const double scale=static_cast<double>(BLOCKSIDELENGTH);
        const int instructionId=api->HumanMove(sn,goal.x*scale,goal.y*scale);
        remember(sn,1,-1,goal,instructionId); return true;
    }
    bool produce(const tagBuilding& b,int actionId,int f,int w,int g,bool tech=false) {
        if(b.Percent<100||b.Project!=0||!available(b.SN,50)||food<f||wood<w||gold<g) return false;
        if(!tech&&freePop<1) return false;
        food-=f; wood-=w; gold-=g; if(!tech) --freePop;
        int engineAction=actionId==BUILDING_MARKET_CRAFT_UPGRADE?BUILDING_MARKET_WOOD_UPGRADE:actionId;
        remember(b.SN,4,actionId,Pos(),api->BuildingAction(b.SN,engineAction));
        if(tech){Tech& t=techs[actionId];t.state=1;t.building=b.SN;t.issued=frame;t.observed=false;}
        return true;
    }
    bool research(const tagBuilding& b,int id,int f,int w=0,int g=0) {
        if(techs[id].state!=0) return false;
        return produce(b,id,f,w,g,true);
    }
    void receipt() {
        for(auto& kv:orders) {
            Order& o=kv.second; if(o.id<0) continue;
            auto it=s->ins_ret.find(o.id); if(it==s->ins_ret.end()) continue;
            if(it->second!=0) {
                ++errors;
                if(trace) trace<<"ERR "<<frame<<" sn="<<kv.first<<" kind="<<o.kind<<" target="<<o.target<<" ret="<<it->second<<'\n';
                if(o.kind==4&&techs.count(o.target)&&techs[o.target].state==1) techs[o.target]=Tech();
                if(o.kind==3&&kv.first==builder){pendingBuild=-1;blockedSites[{int(o.point.x),int(o.point.y)}]=frame+3000;}
            }
            o.id=-1;
        }
        for(auto& kv:techs) {
            Tech& t=kv.second; if(t.state!=1) continue;
            const auto* b=building(t.building);
            if(!b){t=Tech();continue;}
            int engineAction=kv.first==BUILDING_MARKET_CRAFT_UPGRADE?BUILDING_MARKET_WOOD_UPGRADE:kv.first;
            if(b->Project==engineAction) t.observed=true;
            if(t.observed&&b->Project==0) t.state=2;
            if(!t.observed&&frame-t.issued>200) t=Tech();
        }
    }
    void grid() {
        terrain=*s->theMap; width=int(terrain.size());height=width?int(terrain[0].size()):0;
        occupied.assign(width,std::vector<int>(height,0));
        auto mark=[&](int x,int y,int n){for(int i=x;i<x+n;++i)for(int j=y;j<y+n;++j)if(inBounds(i,j))occupied[i][j]=1;};
        for(const auto& b:s->buildings)mark(b.BlockDR,b.BlockUR,sizeOf(b.Type));
        for(const auto& kv:knownBuildings)mark(kv.second.BlockDR,kv.second.BlockUR,sizeOf(kv.second.Type));
        for(const auto& r:s->resources)if(r.Type==RESOURCE_TREE||r.Type==RESOURCE_BUSH||r.Type==RESOURCE_STONE||r.Type==RESOURCE_GOLD)mark(r.BlockDR,r.BlockUR,1);
        reachable.assign(width,std::vector<int>(height,0));
        Pos origin=freePoint(base);std::queue<std::pair<int,int>> q;
        int ox=int(origin.x),oy=int(origin.y);if(!inBounds(ox,oy))return;
        reachable[ox][oy]=1;q.push({ox,oy});
        const int dx[]={1,-1,0,0},dy[]={0,0,1,-1};
        while(!q.empty()){
            auto p=q.front();q.pop();
            for(int k=0;k<4;++k){int x=p.first+dx[k],y=p.second+dy[k];
                if(!open(x,y)||reachable[x][y])continue;
                int h1=terrain[p.first][p.second].height,h2=terrain[x][y].height;
                if(h1>=0&&h2>=0&&h1!=h2)continue;
                reachable[x][y]=1;q.push({x,y});
            }
        }
    }
    bool accessible(Pos p,int radius=1) const {
        for(int dx=-radius;dx<=radius;++dx)for(int dy=-radius;dy<=radius;++dy){int x=int(p.x)+dx,y=int(p.y)+dy;if(inBounds(x,y)&&reachable[x][y])return true;}
        return false;
    }
    int drop(int kind,Pos p,double* dist=nullptr) const {
        int sn=-1;double best=1e20;
        for(const auto& b:s->buildings){
            if(b.Percent<100)continue;
            bool ok=b.Type==BUILDING_CENTER||(kind==HUMAN_GRANARYFOOD?b.Type==BUILDING_GRANARY:b.Type==BUILDING_STOCK);
            if(!ok)continue;double d=distance(p,position(b));if(d<best){best=d;sn=b.SN;}
        }
        if(dist)*dist=best;return sn;
    }
    bool deliver(const tagFarmer& f) {
        if(f.Resource<=0)return false;
        int target=drop(f.ResourceSort,position(f));
        if(target<0)return false;
        if(f.WorkObjectSN!=target||f.NowState==HUMAN_STATE_IDLE)action(f.SN,target,f.WorkObjectSN,f.NowState);
        return true;
    }
    bool buildAt(int type,Pos anchor,int cost) {
        const tagFarmer* f=farmer(builder);
        if(!f||pendingBuild>=0||!available(builder,75)||wood<cost)return false;
        if(deliver(*f))return true;
        int n=sizeOf(type),bx=-1,by=-1;double best=1e20;
        for(int x=std::max(1,int(anchor.x)-12);x<std::min(width-n-1,int(anchor.x)+13);++x)
        for(int y=std::max(1,int(anchor.y)-12);y<std::min(height-n-1,int(anchor.y)+13);++y){
            if(blockedSites[{x,y}]>frame)continue;
            int h=terrain[x][y].height;bool ok=h>=0;
            for(int i=x;i<x+n&&ok;++i)for(int j=y;j<y+n;++j)if(!land(i,j)||occupied[i][j]||terrain[i][j].height!=h){ok=false;break;}
            Pos p(x+n*0.5,y+n*0.5);
            if(!ok||!accessible(p,n)||danger(p,10)||distance(p,base)>30)continue;
            for(const auto& a:s->farmers)if(a.BlockDR>=x&&a.BlockDR<x+n&&a.BlockUR>=y&&a.BlockUR<y+n){ok=false;break;}
            for(const auto& a:s->armies)if(a.BlockDR>=x&&a.BlockDR<x+n&&a.BlockUR>=y&&a.BlockUR<y+n){ok=false;break;}
            if(!ok)continue;
            double v=distance(p,anchor)+0.25*distance(p,position(*f));
            // Leave a one-tile corridor around existing buildings where possible.
            for(const auto& b:s->buildings)if(distance(p,position(b))<(n+sizeOf(b.Type))*0.5+1.5)v+=8;
            v+=((x*17+y*11+buildSequence)%13)*0.03;
            if(v<best){best=v;bx=x;by=y;}
        }
        if(bx<0)return false;
        wood-=cost;pendingBuild=type;buildFrame=frame;buildX=bx;buildY=by;++buildSequence;
        remember(builder,3,type,Pos(bx,by),api->HumanBuild(builder,type,bx,by));return true;
    }
    void construction() {
        if(!farmer(builder)){
            builder=-1;for(const auto& f:s->farmers)if(f.FarmerSort==FARMERTYPE_FARMER&&(builder<0||f.SN<builder))builder=f.SN;
            pendingBuild=-1;
        }
        const tagFarmer* f=farmer(builder);if(!f)return;
        if(pendingBuild>=0){
            const tagBuilding* target=nullptr;
            for(const auto& b:s->buildings)if(b.Type==pendingBuild&&b.BlockDR==buildX&&b.BlockUR==buildY)target=&b;
            if(target&&target->Percent>=100)pendingBuild=-1;
            else if(target){if(f->NowState==HUMAN_STATE_IDLE)action(builder,target->SN,f->WorkObjectSN,f->NowState);return;}
            else if(frame-buildFrame<500)return;
            else pendingBuild=-1;
        }
        // Resume interrupted foundations before adding another project.
        for(const auto& b:s->buildings)if(b.Percent<100&&b.Type!=BUILDING_ARROWTOWER){
            pendingBuild=b.Type;buildX=b.BlockDR;buildY=b.BlockUR;buildFrame=frame;
            action(builder,b.SN,f->WorkObjectSN,f->NowState);return;
        }
        Pos yard=base+direction*5;
        if(s->Human_MaxNum<50&&s->Human_Num+3>=s->Human_MaxNum&&buildAt(BUILDING_HOME,base-direction*4,BUILD_HOUSE_WOOD))return;
        int required=-1,cost=0;
        if(count(BUILDING_ARMYCAMP)==0){required=BUILDING_ARMYCAMP;cost=BUILD_ARMYCAMP_WOOD;}
        else if(count(BUILDING_MARKET)==0){required=BUILDING_MARKET;cost=BUILD_MARKET_WOOD;}
        else if(count(BUILDING_RANGE)==0){required=BUILDING_RANGE;cost=BUILD_RANGE_WOOD;}
        if(required>=0){
            if(buildAt(required,required==BUILDING_MARKET?base:yard,cost))return;
            if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
            return;
        }
        if(center&&center->Project==BUILDING_CENTER_UPGRADE&&wood<180){
            if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
            return;
        }
        // Reserve timber for the first combat technology before discretionary construction.
        if(s->civilizationStage>=3&&!done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)){
            if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
            return;
        }
        if(s->civilizationStage>=3&&count(BUILDING_RANGE)<2){
            if(buildAt(BUILDING_RANGE,yard+direction*3,BUILD_RANGE_WOOD))return;
            if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
            return;
        }
        // Add a depot only when a useful safe resource cluster has a long delivery route.
        Pos depot;double depotValue=0;
        for(const auto& r:s->resources){
            if(r.Cnt<=0||r.Type==RESOURCE_BUSH||r.Type==RESOURCE_STONE||r.Type==RESOURCE_FISH||distance(position(r),base)>23||!accessible(position(r))||danger(position(r),10))continue;
            if(r.Type==RESOURCE_GOLD&&s->civilizationStage<3&&s->Meat<650)continue;
            double d=0;drop(r.Type==RESOURCE_GOLD?HUMAN_GOLD:HUMAN_WOOD,position(r),&d);
            if(d<6)continue;
            int neighbors=0;for(const auto& other:s->resources)if(other.Type==r.Type&&other.Cnt>0&&distance(position(other),position(r))<5)++neighbors;
            double v=std::min(d,12.0)*std::min(neighbors,5);
            if(r.Type==RESOURCE_GAZELLE)v*=1.4;
            if(v>depotValue){depotValue=v;depot=position(r);}
        }
        if(depotValue>=22&&count(BUILDING_STOCK)<3&&wood>=BUILD_STOCK_WOOD+100&&buildAt(BUILDING_STOCK,depot,BUILD_STOCK_WOOD))return;
        if(s->civilizationStage>=3){
            int desired=2;
            if(count(BUILDING_RANGE)<desired&&(done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)||wood>=250)&&buildAt(BUILDING_RANGE,yard+direction*3,BUILD_RANGE_WOOD))return;
        }
        int naturalFood=0;for(const auto& r:s->resources)if((r.Type==RESOURCE_BUSH||r.Type==RESOURCE_GAZELLE)&&r.Cnt>0&&distance(position(r),base)<25&&!danger(position(r),10))naturalFood+=r.Cnt;
        int usefulFarms=0;for(const auto& b:s->buildings)if(b.Type==BUILDING_FARM&&b.Cnt>0)++usefulFarms;
        if(count(BUILDING_MARKET,true)&&naturalFood<700&&usefulFarms<10&&buildAt(BUILDING_FARM,base-direction*3,BUILD_FARM_WOOD))return;
        if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
    }
    void production() {
        if(center&&center->Project==0){
            bool prereq=(count(BUILDING_MARKET,true)>0)+(count(BUILDING_RANGE,true)>0)+(count(BUILDING_STABLE,true)>0)>=2;
            if(s->civilizationStage==2&&food>=BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD&&prereq)
                produce(*center,BUILDING_CENTER_UPGRADE,BUILDING_CENTER_UPGRADE_BRONZEAGE_FOOD,0,0,true);
            else if(s->farmers.size()<20)produce(*center,BUILDING_CENTER_CREATEFARMER,50,0,0);
        }
        if(s->civilizationStage<3)return;
        for(const auto& b:s->buildings)if(b.Type==BUILDING_RANGE){
            if(!done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW))research(b,BUILDING_RANGE_UPGRADE_COMPOSITE_BOW,180,100);
            else produce(b,BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN,40,0,20);
        }
        for(const auto& b:s->buildings)if(b.Type==BUILDING_MARKET&&done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)){
            if(!done(BUILDING_MARKET_WOOD_UPGRADE))research(b,BUILDING_MARKET_WOOD_UPGRADE,120,75);
            else if(count(BUILDING_RANGE,true)>=2&&!done(BUILDING_MARKET_CRAFT_UPGRADE))research(b,BUILDING_MARKET_CRAFT_UPGRADE,170,150);
        }
        if(troops(true,false)>=6)for(const auto& b:s->buildings)if(b.Type==BUILDING_ARMYCAMP){
            if(!done(BUILDING_ARMYCAMP_UPGRADE_BROADSWORD))research(b,BUILDING_ARMYCAMP_UPGRADE_BROADSWORD,140,0,50);
            else if(troops(false,true)<5)produce(b,BUILDING_ARMYCAMP_CREATE_BROADSWORD,35,0,15);
        }
    }
    int selectResource(const tagFarmer& f,int role) {
        Worker& w=workers[f.SN];int target=-1;double best=1e20;
        for(const auto& r:s->resources){
            bool ok=role==1?r.Type==RESOURCE_TREE:role==2?r.Type==RESOURCE_GOLD:(r.Type==RESOURCE_BUSH||r.Type==RESOURCE_GAZELLE||(r.Blood<=0&&(r.Type==RESOURCE_ELEPHANT||r.Type==RESOURCE_LION)));
            if(!ok||r.Cnt<=0||!accessible(position(r))||distance(position(r),base)>29||danger(position(r),9))continue;
            if((w.blocked==r.SN&&frame<w.avoidUntil)||blockedResources[{f.SN,r.SN}]>frame)continue;
            int used=useCount[r.SN]-(f.WorkObjectSN==r.SN?1:0);if(used>=(r.Type==RESOURCE_TREE?1:3))continue;
            double d=0;int kind=role==1?HUMAN_WOOD:role==2?HUMAN_GOLD:r.Type==RESOURCE_BUSH?HUMAN_GRANARYFOOD:HUMAN_STOCKFOOD;
            drop(kind,position(r),&d);
            double v=distance(position(f),position(r))+d*1.7+used*5;
            if(r.Type==RESOURCE_GAZELLE&&r.Blood>0)v+=2;
            if(v<best){best=v;target=r.SN;}
        }
        if(role==0)for(const auto& b:s->buildings)if(b.Type==BUILDING_FARM&&b.Percent>=100&&b.Cnt>0&&(useCount[b.SN]==0||f.WorkObjectSN==b.SN)&&!danger(position(b),9)&&blockedResources[{f.SN,b.SN}]<=frame){
            double d=0;drop(HUMAN_GRANARYFOOD,position(b),&d);double v=distance(position(f),position(b))+d+3;
            if(v<best){best=v;target=b.SN;}
        }
        return target;
    }
    bool assign(const tagFarmer& f,int role) {
        int target=selectResource(f,role);if(target<0)return false;
        if(action(f.SN,target,f.WorkObjectSN,f.NowState)){workers[f.SN].role=role;workers[f.SN].target=target;workers[f.SN].progress=frame;++useCount[target];return true;}
        return false;
    }
    void economy() {
        for(auto it=workers.begin();it!=workers.end();)if(!farmer(it->first))it=workers.erase(it);else ++it;
        useCount.clear();for(const auto& f:s->farmers)if(f.WorkObjectSN>=0)++useCount[f.WorkObjectSN];
        int n=std::max(0,int(s->farmers.size())-1);
        int wantGold=(s->civilizationStage>=3?(s->Gold<100?4:3):(center&&center->Project==BUILDING_CENTER_UPGRADE?2:0));
        int wantWood=std::min(n/2,s->civilizationStage<3?7:(s->Wood<250?7:5));
        int wanted[3]={std::max(3,n-wantWood-wantGold),wantWood,wantGold};
        int counts[3]={0,0,0};
        for(const auto& f:s->farmers)if(f.SN!=builder&&workers.count(f.SN))++counts[workers[f.SN].role];
        for(const auto& f:s->farmers){
            if(f.SN==builder||f.FarmerSort!=FARMERTYPE_FARMER)continue;
            bool fresh=!workers.count(f.SN);Worker& w=workers[f.SN];
            if(fresh){w.point=position(f);w.progress=frame;w.cargo=f.Resource;}
            if(distance(w.point,position(f))>0.3||w.cargo!=f.Resource){w.point=position(f);w.cargo=f.Resource;w.progress=frame;}
            bool stuck=frame-w.progress>300;
            const tagBuilding* current=building(f.WorkObjectSN);
            if(current&&current->Percent<100)continue;
            if(danger(position(f),5)){
                const tagArmy* threat=nullptr;for(const auto& e:s->enemy_armies)if(e.WorkObjectSN==f.SN){threat=&e;break;}
                if(threat){move(f,position(f)+unit(position(f)-position(*threat))*5,40);continue;}
            }
            int role=w.role;
            bool exhausted=false;
            if(current&&current->Type==BUILDING_FARM&&current->Cnt<=0)exhausted=true;
            for(const auto& r:s->resources)if(r.SN==f.WorkObjectSN&&r.Cnt<=0)exhausted=true;
            bool adjust=!fresh&&counts[w.role]>wanted[w.role]&&f.Resource==0&&frame-rebalanceFrame>=40;
            if(fresh||f.NowState==HUMAN_STATE_IDLE||stuck||exhausted||adjust){
                int bestDeficit=-100;
                for(int candidate=0;candidate<3;++candidate){
                    int deficit=wanted[candidate]-counts[candidate];
                    if(deficit>bestDeficit){bestDeficit=deficit;role=candidate;}
                }
            }else continue;
            if(stuck){w.blocked=f.WorkObjectSN;w.avoidUntil=frame+1800;blockedResources[{f.SN,f.WorkObjectSN}]=frame+1800;w.progress=frame;}
            if(deliver(f))continue;
            if(!fresh)--counts[w.role];
            if(!assign(f,role)){
                for(int alternative=0;alternative<3;++alternative)if(alternative!=role&&assign(f,alternative))break;
            }
            if(adjust)rebalanceFrame=frame;
            ++counts[w.role];
        }
    }
    void towers() {
        for(const auto& b:s->buildings)if(b.Type==BUILDING_ARROWTOWER&&b.Percent>=100){
            const tagArmy* best=nullptr;double score=1e20;
            double range=7+(done(BUILDING_MARKET_WOOD_UPGRADE)?1:0)+(done(BUILDING_MARKET_CRAFT_UPGRADE)?1:0);
            for(const auto& e:s->enemy_armies){
                double d=distance(position(b),position(e));if(e.Blood<=0||d>range)continue;
                bool unlocked=e.WorkObjectSN!=b.SN;
                double v=(unlocked?0:100)+d+(ranged(e.Sort)?3:0);
                if(priest&&e.WorkObjectSN==priest->SN)v-=10;
                if(v<score){score=v;best=&e;}
            }
            if(!best)continue;
            const auto* current=enemy(b.Project);
            if(best->WorkObjectSN==b.SN&&current&&distance(position(b),position(*current))<=range)continue;
            action(b.SN,best->SN,b.Project,b.Project>=0?HUMAN_STATE_ATTACKING:HUMAN_STATE_IDLE,20);
        }
    }
    void scouting() {
        if(phase==0)return;
        if(!army(scout)||recovering.count(scout)||priestGuard.count(scout)){
            scout=-1;
            for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&!priestGuard.count(a.SN)&&a.Blood*4>=a.MaxBlood*3&&(scout<0||a.SN<scout))scout=a.SN;
        }
    }
    void transition(int next) { if(phase!=next){phase=next;phaseFrame=frame;if(trace)trace<<"PHASE "<<frame<<' '<<phase<<'\n';} }
    Pos slot(const tagArmy& a,int index,bool rear) const {
        Pos side(-direction.y,direction.x);
        if(!ranged(a.Sort))return front+direction*2+side*((index%5-2)*1.25);
        return front-direction*(rear?3:1)-direction*(index/7)*1.1+side*((index%7-3)*1.3);
    }
    void campaign() {
        bool baseThreat=false;for(const auto& e:s->enemy_armies)if(distance(position(e),base)<26){baseThreat=true;lastThreat=frame;}
        if(phase==0){
            const tagBuilding* tower=nullptr;
            for(const auto& b:s->buildings)if(b.Type==BUILDING_ARROWTOWER)tower=&b;
            front=tower?position(*tower):base+direction*3;
            if(frame>=23000&&s->civilizationStage>=3&&!baseThreat&&frame-lastThreat>300&&troops(true,false)>=6&&troops(true,true)>=10){front=freePoint(base+direction*9);marchRoute.push_back(front);transition(1);}
        }
        int close=0,total=0;for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&a.Blood>0){++total;if(distance(position(a),front)<8)++close;}
        int rallyNeed=frame>=36000?8:10;
        if(phase==1&&priest&&close>=rallyNeed&&close*5>=total*3&&distance(position(*priest),front)<11)transition(2);
        if(phase>=2){
            Pos destination(width-base.x,height-base.y);
            for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE)destination=position(kv.second);
            if(frame-planFrame>=70){plannedRoute=route(front,destination);planFrame=frame;}
            direction=unit(plannedRoute.empty()?destination-front:plannedRoute[std::min(3,int(plannedRoute.size())-1)]-front);
            bool contact=false,nearEnemySite=false;
            for(const auto& e:s->enemy_armies){double d=distance(position(e),front);if(d<7)contact=true;if(d<18)nearEnemySite=true;}
            for(const auto& kv:knownBuildings)if(distance(position(kv.second),front)<26)nearEnemySite=true;
            int readyForSite=0;for(const auto& a:s->armies)if(ranged(a.Sort)&&distance(position(a),front)<10&&a.Blood*2>a.MaxBlood)++readyForSite;
            bool siteReady=!nearEnemySite||readyForSite>=(frame>=36000?6:12);
            if(!contact&&siteReady&&close>=std::min(total,frame>=36000?8:10)&&priest&&distance(position(*priest),front)<11&&frame-advanceFrame>70){
                Pos next=front;
                int walked=0;
                for(const auto& step:plannedRoute){
                    if(!open(int(step.x),int(step.y)))break;
                    next=step;
                    if(marchRoute.empty()||distance(marchRoute.back(),step)>0.1)marchRoute.push_back(step);
                    if(++walked>=3)break;
                }
                if(distance(next,front)>0.2){front=next;planFrame=-1000;}
                advanceFrame=frame;
            }
            if(total<(frame>=36000?7:9))transition(1);
            int readyRanged=0,healthy=0;
            for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&distance(position(a),front)<9&&a.Blood*2>a.MaxBlood){++healthy;if(ranged(a.Sort))++readyRanged;}
            if(distance(front,destination)<10&&s->enemy_armies.size()<3&&
               (readyRanged>=12||(frame>=36000&&readyRanged>=6&&healthy>=8)))transition(3);
        }
        int bowIndex=0,meleeIndex=0;
        std::vector<const tagArmy*> soldiers;for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&a.Blood>0)soldiers.push_back(&a);
        std::sort(soldiers.begin(),soldiers.end(),[](const tagArmy* a,const tagArmy* b){return a->SN<b->SN;});
        for(auto it=priestGuard.begin();it!=priestGuard.end();)if(!army(*it)||recovering.count(*it))it=priestGuard.erase(it);else ++it;
        if(priest)while(priestGuard.size()<3){
            const tagArmy* candidate=nullptr;double best=1e20;
            for(const auto* a:soldiers)if(!priestGuard.count(a->SN)&&!recovering.count(a->SN)&&a->Blood*2>a->MaxBlood){
                double value=distance(position(*a),position(*priest))+(ranged(a->Sort)?3:0);
                if(value<best){best=value;candidate=a;}
            }
            if(!candidate)break;priestGuard.insert(candidate->SN);
        }
        for(const auto* soldier:soldiers){
            const auto& a=*soldier;
            Pos ap=position(a);bool rangeUnit=ranged(a.Sort);
            Pos goal=slot(a,rangeUnit?bowIndex++:meleeIndex++,phase>0);
            if(priest&&priestGuard.count(a.SN))goal=position(*priest)+direction*2+Pos(-direction.y,direction.x)*((a.SN%3)-1)*1.5;
            if(a.Blood*5<a.MaxBlood*2)recovering.insert(a.SN);
            if(a.Blood*5>=a.MaxBlood*4)recovering.erase(a.SN);
            if(recovering.count(a.SN)&&priest&&distance(ap,position(*priest))<15){
                Pos retreat=priestAnchor()-direction+Pos(-direction.y,direction.x)*((a.SN%3)-1);
                if(distance(ap,retreat)>1.4)move(a,retreat,30);
                continue;
            }
            if(a.SN==scout&&phase>=2){
                Pos probe=front;int steps=0;
                for(const auto& step:plannedRoute){probe=step;if(++steps>=6)break;}
                if(!danger(probe,7))goal=probe;
            }
            const tagArmy* target=nullptr;double best=1e20;
            double ownRange=a.Sort==AT_COMPOSITE_BOWMAN?7+(done(BUILDING_MARKET_WOOD_UPGRADE)?1:0)+(done(BUILDING_MARKET_CRAFT_UPGRADE)?1:0):a.Sort==AT_STONE_THROWER?10:rangeUnit?7:1;
            for(const auto& e:s->enemy_armies){
                Pos ep=position(e);double d=distance(ap,ep);
                bool huntsPriest=priest&&e.WorkObjectSN==priest->SN;
                double acquire=rangeUnit?ownRange+(huntsPriest?9:phase>=1?4:2):huntsPriest?18:phase>=1?12:10;
                if(e.Blood<=0||(!huntsPriest&&distance(ep,front)>15)||d>acquire)continue;
                double value=d*2+(ranged(e.Sort)?-9:0);
                if(huntsPriest)value-=1000;
                if(e.Sort==AT_STONE_THROWER)value-=4;
                value+=e.Blood*0.01;
                if(value<best){best=value;target=&e;}
            }
            if(rangeUnit){
                if(frame<dodgeUntil[a.SN])continue;
                const tagArmy* artillery=nullptr;
                for(const auto& e:s->enemy_armies)if(e.Sort==AT_STONE_THROWER&&e.WorkObjectSN==a.SN&&distance(ap,position(e))<13){artillery=&e;break;}
                if(artillery&&frame-dodgeFrame[a.SN]>=80){
                    Pos side=unit(Pos(-(ap-position(*artillery)).y,(ap-position(*artillery)).x));
                    if(move(a,ap+side*((a.SN%2)?2.2:-2.2),18)){dodgeFrame[a.SN]=frame;dodgeUntil[a.SN]=frame+22;continue;}
                }
                const tagArmy* closeEnemy=nullptr;double nearest=1e20;
                for(const auto& e:s->enemy_armies)if(!ranged(e.Sort)&&e.Blood>0&&distance(ap,position(e))<nearest){nearest=distance(ap,position(e));closeEnemy=&e;}
                if(closeEnemy&&nearest<3.5&&closeEnemy->WorkObjectSN==a.SN){move(a,ap+unit(ap-position(*closeEnemy))*3,22);continue;}
            }
            if(target){action(a.SN,target->SN,a.WorkObjectSN,a.NowState,18);continue;}
            if(phase>=2){
                const tagBuilding* tower=nullptr;double dmin=12;
                for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER){double d=distance(ap,position(kv.second));if(d<dmin){dmin=d;tower=&kv.second;}}
                if(tower){action(a.SN,tower->SN,a.WorkObjectSN,a.NowState,30);continue;}
            }
            if(distance(ap,goal)>2.0||a.NowState==HUMAN_STATE_ATTACKING)move(a,goal,45);
        }
    }
    bool safeConversionRoute(const tagBuilding& factory) const {
        if(!priest)return false;
        std::vector<Pos> approach=route(position(*priest),position(factory));
        approach.push_back(position(*priest));approach.push_back(position(factory));
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER){
            bool crosses=false;for(const auto& point:approach)if(distance(point,position(kv.second))<9){crosses=true;break;}
            if(!crosses)continue;
            const tagArmy* tank=army(kv.second.Project);
            if(!tank||tank->Sort==AT_PRIEST||tank->MaxBlood<70||tank->Blood*2<tank->MaxBlood)return false;
        }
        return true;
    }
    void priestControl() {
        if(!priest)return;
        const tagArmy& p=*priest;Pos pp=position(p);
        Pos anchor=priestAnchor();
        const tagArmy* current=enemy(p.WorkObjectSN);
        const tagArmy* threat=nullptr;double nearest=1e20;
        for(const auto& e:s->enemy_armies)if(e.Blood>0){double d=distance(pp,position(e));if(d<nearest){nearest=d;threat=&e;}}
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER&&kv.second.Project==p.SN&&distance(pp,position(kv.second))<10){
            if(p.WorkObjectSN==kv.first||p.Blood<60||phase!=3){
                priestRetreat=freePoint(pp+unit(pp-position(kv.second))*5,p.SN);priestRetreatUntil=frame+125;
                move(p,priestRetreat,15);return;
            }
        }
        int escorts=0;for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&distance(pp,position(a))<8)++escorts;
        const tagArmy* hunter=nullptr;double hunterDistance=1e20;
        for(const auto& e:s->enemy_armies)if(e.Blood>0&&e.WorkObjectSN==p.SN){
            double d=distance(pp,position(e));if(d<(ranged(e.Sort)?11.5:4.5)&&d<hunterDistance){hunter=&e;hunterDistance=d;}
        }
        bool activeConversion=current&&p.NowState==HUMAN_STATE_ATTACKING&&distance(pp,position(*current))<11.8;
        if(hunter&&(!activeConversion||p.Blood<60||(!ranged(hunter->Sort)&&hunterDistance<2.2))){
            priestRetreat=freePoint(pp+unit(pp-position(*hunter))*4,p.SN);priestRetreatUntil=frame+125;
            move(p,priestRetreat,18);return;
        }
        if(frame<priestRetreatUntil){move(p,priestRetreat,45);return;}
        // Preserve conversion once the target is valid and in range.
        if(current&&p.NowState==HUMAN_STATE_ATTACKING&&distance(pp,position(*current))<11.8)return;
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE&&p.WorkObjectSN==kv.first&&p.NowState!=HUMAN_STATE_IDLE)return;
        if(phase==0&&frame<4500&&!danger(pp,9)){
            Pos side(-direction.y,direction.x);
            Pos scoutPoint=base+direction*6+side*(frame<1800?5:-5);
            if(!danger(scoutPoint,10)&&distance(scoutPoint,base)<12){move(p,scoutPoint,60);return;}
        }
        if(phase>=1&&(escorts<4||distance(pp,anchor)>4.5)){move(p,anchor,40);return;}
        if(p.ConvertCooldown==0){
            const tagArmy* best=nullptr;double score=-1e20;
            for(const auto& e:s->enemy_armies){
                double d=distance(pp,position(e));if(e.Blood<=0||d>11.3||distance(position(e),front)>15)continue;
                if(phase==0&&!ranged(e.Sort)){
                    bool towerLock=false;for(const auto& b:s->buildings)if(b.Type==BUILDING_ARROWTOWER&&e.WorkObjectSN==b.SN)towerLock=true;
                    if(!towerLock&&d>5)continue;
                }
                double v=e.Blood-d*2;
                if(e.Sort==AT_HOPLITE||e.Sort==AT_CAVALRY)v+=70;
                if(ranged(e.Sort)&&phase==0)v+=140;
                if(e.WorkObjectSN==p.SN)v+=80;
                if(v>score){score=v;best=&e;}
            }
            if(best){action(p.SN,best->SN,p.WorkObjectSN,p.NowState,20);return;}
        }
        if(phase==3&&escorts>=6){
            for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE){
                if((s->enemy_armies.empty()||nearest>11)&&safeConversionRoute(kv.second)){action(p.SN,kv.first,p.WorkObjectSN,p.NowState,35);return;}
            }
        }
        const tagArmy* healing=army(p.WorkObjectSN);
        if(healing&&healing->Blood<healing->MaxBlood&&distance(position(*healing),anchor)<7&&distance(pp,position(*healing))<2.5&&nearest>9)return;
        if(nearest>9){
            const tagArmy* injured=nullptr;double missing=0;
            for(const auto& ally:s->armies)if(ally.Sort!=AT_PRIEST&&distance(position(ally),anchor)<6&&distance(pp,position(ally))<3&&ally.Blood<ally.MaxBlood){
                double need=double(ally.MaxBlood-ally.Blood)/ally.MaxBlood;if(need>missing){missing=need;injured=&ally;}
            }
            if(injured){action(p.SN,injured->SN,p.WorkObjectSN,p.NowState,30);return;}
        }
        if(distance(pp,anchor)>1.6||p.NowState==HUMAN_STATE_ATTACKING||p.NowState==HUMAN_STATE_WORKING)move(p,anchor,45);
    }
    void logging() {
        if(frame-logFrame<250)return;logFrame=frame;
        if(!trace)return;
        trace<<"STAT "<<frame<<" phase="<<phase<<" age="<<s->civilizationStage<<" res="<<s->Meat<<','<<s->Wood<<','<<s->Gold
             <<" workers="<<s->farmers.size()<<" pop="<<s->Human_Num<<'/'<<s->Human_MaxNum
             <<" bow="<<troops(true,false)<<" melee="<<troops(false,true)<<" priest="<<(priest?priest->Blood:0)
             <<" enemy="<<s->enemy_armies.size()<<" errors="<<errors<<" build="<<pendingBuild<<" front="<<front.x<<','<<front.y;
        if(priest)trace<<" priestPos="<<position(*priest).x<<','<<position(*priest).y<<" priestTask="<<priest->WorkObjectSN<<" cooldown="<<priest->ConvertCooldown<<" anchor="<<priestAnchor().x<<','<<priestAnchor().y<<" order="<<orders[priest->SN].point.x<<','<<orders[priest->SN].point.y;
        int idle=0;for(const auto& f:s->farmers)if(f.NowState==HUMAN_STATE_IDLE)++idle;
        int roles[3]={0,0,0};for(const auto& kv:workers)if(kv.first!=builder)++roles[kv.second.role];
        trace<<" idle="<<idle<<" roles="<<roles[0]<<','<<roles[1]<<','<<roles[2]<<" tech="<<done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)<<" ranges="<<count(BUILDING_RANGE)<<'\n';
        if(frame%1008<260){
            for(const auto& b:s->buildings)trace<<"BUILD "<<frame<<" sn="<<b.SN<<" type="<<b.Type<<" pct="<<b.Percent<<" pos="<<b.BlockDR<<','<<b.BlockUR<<" cnt="<<b.Cnt<<" project="<<b.Project<<'\n';
            for(const auto& f:s->farmers)trace<<"WORK "<<frame<<" sn="<<f.SN<<" role="<<workers[f.SN].role<<" pos="<<position(f).x<<','<<position(f).y<<" state="<<f.NowState<<" target="<<f.WorkObjectSN<<" cargo="<<f.Resource<<'\n';
            for(const auto& a:s->armies)trace<<"ARMY "<<frame<<" sn="<<a.SN<<" type="<<a.Sort<<" hp="<<a.Blood<<" pos="<<position(a).x<<','<<position(a).y<<" state="<<a.NowState<<" target="<<a.WorkObjectSN<<'\n';
            for(const auto& kv:knownBuildings)trace<<"ENEMYBUILD "<<frame<<" sn="<<kv.first<<" type="<<kv.second.Type<<" hp="<<kv.second.Blood<<" pos="<<position(kv.second).x<<','<<position(kv.second).y<<'\n';
            for(const auto& a:s->enemy_armies)trace<<"ENEMYARMY "<<frame<<" sn="<<a.SN<<" type="<<a.Sort<<" hp="<<a.Blood<<" pos="<<position(a).x<<','<<position(a).y<<" target="<<a.WorkObjectSN<<'\n';
        }
        if(phase>=1){
            std::ofstream mapTrace("known_terrain.txt");
            mapTrace<<width<<' '<<height<<' '<<frame<<'\n';
            for(int y=0;y<height;++y){for(int x=0;x<width;++x)mapTrace<<terrain[x][y].type<<','<<terrain[x][y].height<<','<<occupied[x][y]<<' ';mapTrace<<'\n';}
        }
        trace.flush();
    }
public:
    void tick(AI& a,const tagInfo& snapshot) {
        if(!snapshot.theMap||snapshot.buildings.empty()||snapshot.GameFrame==last)return;
        s=&snapshot;api=&a;frame=s->GameFrame;
        if(last<0){trace.open("rewrite_trace.log",std::ios::out);if(trace)trace<<"VERSION rewrite-r14\n";}
        if(last>=0&&frame-last<6)return;
        last=frame;sent.clear();center=nullptr;priest=nullptr;
        for(const auto& b:s->buildings)if(b.Type==BUILDING_CENTER)center=&b;
        for(const auto& u:s->armies)if(u.Sort==AT_PRIEST)priest=&u;
        if(!center||!priest){
            if(frame-logFrame>=250){
                logFrame=frame;trace<<"MISSING "<<frame<<" center="<<(center?center->SN:-1)<<" priest="<<(priest?priest->SN:-1);
                for(const auto& b:s->buildings)trace<<" b="<<b.SN<<':'<<b.Type;
                for(const auto& u:s->armies)trace<<" a="<<u.SN<<':'<<u.Sort;
                trace<<'\n';trace.flush();
            }
            return;
        }
        base=position(*center);
        if(length(direction)<0.1)direction=unit(Pos(s->theMap->size()*0.5,(*s->theMap)[0].size()*0.5)-base);
        for(const auto& b:s->enemy_buildings)if(b.Blood>0)knownBuildings[b.SN]=b;
        for(auto it=knownBuildings.begin();it!=knownBuildings.end();){
            bool exists=false;for(const auto& b:s->enemy_buildings)if(b.SN==it->first&&b.Blood>0)exists=true;
            if(!exists)it=knownBuildings.erase(it);else ++it;
        }
        wood=s->Wood;food=s->Meat;gold=s->Gold;freePop=std::max(0,int(s->Human_MaxNum-s->Human_Num));
        if(frame<30){trace<<"BOOT "<<frame<<" before-grid\n";trace.flush();}
        receipt();grid();
        if(frame<30){trace<<"BOOT "<<frame<<" after-grid\n";trace.flush();}
        towers();campaign();priestControl();scouting();
        if(frame<30){trace<<"BOOT "<<frame<<" before-economy\n";trace.flush();}
        production();economy();construction();logging();
    }
};
}

void UsrAI::processData()
{
    tagInfo snapshot=getInfo();
    static newaoe_ai::Strategy strategy;
    strategy.tick(*this,snapshot);
}
