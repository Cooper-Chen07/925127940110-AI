#include "UsrAI.h"
#include <algorithm>
#include <cmath>
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
double attackDistance(Pos a,Pos b) { return std::max(std::abs(a.x-b.x),std::abs(a.y-b.y)); }
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
    int frame=0, last=-1, builder=-1, scout=-1;
    int openingScoutStep=0, openingScoutFrame=0;
    int phase=0, phaseFrame=0, lastThreat=0, advanceFrame=0;
    int priestRetreatUntil=0;
    int healSN=-1,healBlood=-1,healFrame=0,healApproachSN=-1,healApproachUntil=0;
    Pos healApproach;
    Pos priestRetreat;
    int wood=0, food=0, gold=0, freePop=0;
    Pos base, front, direction;
    std::map<int,Order> orders;
    std::map<int,Worker> workers;
    std::map<std::pair<int,int>,int> blockedResources;
    std::map<std::pair<int,int>,int> blockedSites;
    std::map<int,int> blockedFoundations;
    std::map<std::pair<int,int>,int> blockedPriestSteps;
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
    int watchedBuild=-1,watchedPercent=-1,buildProgress=0;
    Pos buildWorkerPoint;
    int buildSequence=0, errors=0, rebalanceFrame=-1000;
    const tagArmy* priest=nullptr;
    const tagBuilding* center=nullptr;

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
        int n=0; for(const auto& b:s->buildings) if(b.Type==type&&(!finished||b.Percent>=100)) {
            auto blocked=blockedFoundations.find(b.SN);
            if(b.Percent<100&&blocked!=blockedFoundations.end()&&blocked->second>frame)continue;
            ++n;
        } return n;
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
        for(const auto& kv:knownBuildings) if(kv.second.Type==BUILDING_ARROWTOWER&&attackDistance(p,position(kv.second))<11) return true;
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
    std::vector<Pos> route(Pos from,Pos to,bool priestRoute=false) const {
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
                auto blocked=blockedPriestSteps.find({nx,ny});
                if(priestRoute&&blocked!=blockedPriestSteps.end()&&blocked->second>frame)continue;
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
        if(priest&&h.SN==priest->SN&&phase>=1&&distance(position(h),goal)>4){
            auto previous=orders.find(h.SN);
            if(previous!=orders.end()&&previous->second.kind==1&&previous->second.id<0&&h.NowState==HUMAN_STATE_IDLE&&distance(position(h),previous->second.point)>1.2){
                Pos failed=previous->second.point;
                blockedPriestSteps[{int(failed.x),int(failed.y)}]=frame+6000;
            }
            std::vector<Pos> steps=route(position(h),goal,true);
            if(!steps.empty())goal=steps[std::min(2,int(steps.size())-1)];
        }
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
        std::queue<std::pair<int,int>> q;
        auto seed=[&](Pos origin){
            int x=int(origin.x),y=int(origin.y);if(!inBounds(x,y)||reachable[x][y])return;
            reachable[x][y]=1;q.push({x,y});
        };
        seed(freePoint(base,-1,false));
        for(const auto& f:s->farmers)seed(freePoint(position(f),f.SN,false));
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
        for(int x=std::max(1,int(anchor.x)-20);x<std::min(width-n-1,int(anchor.x)+21);++x)
        for(int y=std::max(1,int(anchor.y)-20);y<std::min(height-n-1,int(anchor.y)+21);++y){
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
            else if(target){
                if(watchedBuild!=target->SN||watchedPercent!=target->Percent||distance(buildWorkerPoint,position(*f))>0.3){
                    watchedBuild=target->SN;watchedPercent=target->Percent;buildWorkerPoint=position(*f);buildProgress=frame;
                }
                if(frame-buildProgress>600){
                    blockedFoundations[target->SN]=frame+6000;
                    blockedSites[{buildX,buildY}]=frame+6000;
                    pendingBuild=-1;watchedBuild=-1;
                    // A different worker may reach the next site from another side.
                    const tagFarmer* replacement=nullptr;double nearest=1e20;
                    for(const auto& candidate:s->farmers)if(candidate.SN!=builder&&candidate.FarmerSort==FARMERTYPE_FARMER&&candidate.Resource==0){
                        double d=distance(position(candidate),base);if(d<nearest){nearest=d;replacement=&candidate;}
                    }
                    assign(*f,1);
                    if(replacement)builder=replacement->SN;
                    return;
                }
                if(f->NowState==HUMAN_STATE_IDLE)action(builder,target->SN,f->WorkObjectSN,f->NowState);
                return;
            }
            else if(frame-buildFrame<500)return;
            else pendingBuild=-1;
        }
        // Resume interrupted foundations before adding another project.
        for(const auto& b:s->buildings)if(b.Percent<100&&b.Type!=BUILDING_ARROWTOWER&&blockedFoundations[b.SN]<=frame){
            pendingBuild=b.Type;buildX=b.BlockDR;buildY=b.BlockUR;buildFrame=frame;
            action(builder,b.SN,f->WorkObjectSN,f->NowState);return;
        }
        Pos yard=base+direction*5;
        if(s->Human_MaxNum<50&&s->Human_Num+3>=s->Human_MaxNum&&buildAt(BUILDING_HOME,base-direction*4,BUILD_HOUSE_WOOD))return;
        int foodWorkers=0;for(const auto& kv:workers)if(kv.first!=builder&&kv.second.role==0)++foodWorkers;
        int naturalFood=0;for(const auto& r:s->resources)if((r.Type==RESOURCE_BUSH||r.Type==RESOURCE_GAZELLE)&&r.Cnt>0&&accessible(position(r))&&distance(position(r),base)<25&&!danger(position(r),10))naturalFood+=r.Cnt;
        int usefulFarms=0;for(const auto& b:s->buildings)if(b.Type==BUILDING_FARM&&b.Cnt>0&&blockedFoundations[b.SN]<=frame)++usefulFarms;
        // Restore food before reserving timber for technologies or optional depots.
        if(count(BUILDING_MARKET,true)&&food<200&&foodWorkers<6&&(naturalFood<700||foodWorkers<3)&&usefulFarms<10&&buildAt(BUILDING_FARM,base-direction*3,BUILD_FARM_WOOD))return;
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
            if(wood<BUILD_RANGE_WOOD){
                if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
                return;
            }
        }
        int pendingEconomyTech=!done(BUILDING_MARKET_WOOD_UPGRADE)?BUILDING_MARKET_WOOD_UPGRADE:
            !done(BUILDING_MARKET_CRAFT_UPGRADE)?BUILDING_MARKET_CRAFT_UPGRADE:-1;
        if(s->civilizationStage>=3&&troops(true,false)>=6&&pendingEconomyTech>=0&&
           (pendingEconomyTech!=BUILDING_MARKET_CRAFT_UPGRADE||count(BUILDING_RANGE,true)>=2)&&techs[pendingEconomyTech].state==0){
            if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);
            return;
        }
        if(s->civilizationStage>=3&&s->Gold<60&&s->Meat>300&&count(BUILDING_STOCK)<3){
            for(const auto& r:s->resources)if(r.Type==RESOURCE_GOLD&&r.Cnt>0&&useCount[r.SN]>0&&distance(position(r),base)<29&&!danger(position(r),10)){
                double delivery=0;drop(HUMAN_GOLD,position(r),&delivery);
                if(delivery<=8)continue;
                if(buildAt(BUILDING_STOCK,position(r),BUILD_STOCK_WOOD))return;
                if(wood<BUILD_STOCK_WOOD){if(f->NowState==HUMAN_STATE_IDLE&&!deliver(*f))assign(*f,1);return;}
            }
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
        if(s->civilizationStage<3){
            int defenders=0;for(const auto& a:s->armies)if(ranged(a.Sort)&&a.Blood>0)++defenders;
            // A slow economy still needs ranged support before the second wave.
            if(frame>=7000&&defenders<4&&food>=100&&wood>=40)
                for(const auto& b:s->buildings)if(b.Type==BUILDING_RANGE)
                    if(produce(b,BUILDING_RANGE_CREATE_BOWMAN,BUILDING_RANGE_CREATE_BOWMAN_FOOD,BUILDING_RANGE_CREATE_BOWMAN_WOOD,0))break;
            return;
        }
        int reserveTechFood=0;
        for(const auto& b:s->buildings)if(b.Type==BUILDING_MARKET&&done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW)){
            int id=!done(BUILDING_MARKET_WOOD_UPGRADE)?BUILDING_MARKET_WOOD_UPGRADE:
                count(BUILDING_RANGE,true)>=2&&!done(BUILDING_MARKET_CRAFT_UPGRADE)?BUILDING_MARKET_CRAFT_UPGRADE:-1;
            if(id>=0&&troops(true,false)>=6&&techs[id].state==0){
                bool issued=research(b,id,id==BUILDING_MARKET_WOOD_UPGRADE?120:170,id==BUILDING_MARKET_WOOD_UPGRADE?75:150);
                if(!issued)reserveTechFood=id==BUILDING_MARKET_WOOD_UPGRADE?120:170;
            }
        }
        for(const auto& b:s->buildings)if(b.Type==BUILDING_RANGE){
            if(!done(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW))research(b,BUILDING_RANGE_UPGRADE_COMPOSITE_BOW,180,100);
            else if(food>=reserveTechFood+40||troops(true,true)<10)produce(b,BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN,40,0,20);
        }
        if(troops(true,false)>=6)for(const auto& b:s->buildings)if(b.Type==BUILDING_ARMYCAMP){
            if(!done(BUILDING_ARMYCAMP_UPGRADE_BROADSWORD)){
                if(food>=reserveTechFood+140)research(b,BUILDING_ARMYCAMP_UPGRADE_BROADSWORD,140,0,50);
            }else if(troops(false,true)<5&&food>=reserveTechFood+35)produce(b,BUILDING_ARMYCAMP_CREATE_BROADSWORD,35,0,15);
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
                double d=attackDistance(position(b),position(e));if(e.Blood<=0||d>range)continue;
                bool unlocked=e.WorkObjectSN!=b.SN;
                double v=(unlocked?0:100)+d+(ranged(e.Sort)?3:0);
                if(priest&&e.WorkObjectSN==priest->SN)v-=10;
                if(v<score){score=v;best=&e;}
            }
            if(!best)continue;
            const auto* current=enemy(b.Project);
            if(best->WorkObjectSN==b.SN&&current&&attackDistance(position(b),position(*current))<=range)continue;
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
    void transition(int next) { if(phase!=next){phase=next;phaseFrame=frame;} }
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
            bool factoryKnown=false;
            for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE){destination=position(kv.second);factoryKnown=true;}
            if(!factoryKnown){
                const tagBuilding* clue=nullptr;double nearest=1e20;
                for(const auto& kv:knownBuildings){double d=distance(front,position(kv.second));if(d<nearest){nearest=d;clue=&kv.second;}}
                if(clue)destination=position(*clue);
                else if(distance(front,destination)<8){
                    Pos best=destination;double score=1e20;
                    const int dx[]={1,-1,0,0},dy[]={0,0,1,-1};
                    for(int x=1;x<width-1;++x)for(int y=1;y<height-1;++y)if(open(x,y)){
                        Pos p(x+0.5,y+0.5);double d=distance(front,p);if(d<3||d>25)continue;
                        int fog=0;for(int k=0;k<4;++k)if(terrain[x+dx[k]][y+dy[k]].type==0)++fog;
                        if(!fog)continue;
                        double value=d+distance(p,destination)*0.4-fog*1.5;
                        if(value<score){score=value;best=p;}
                    }
                    destination=best;
                }
            }
            if(frame-planFrame>=70){plannedRoute=route(front,destination);planFrame=frame;}
            direction=unit(plannedRoute.empty()?destination-front:plannedRoute[std::min(3,int(plannedRoute.size())-1)]-front);
            bool contact=false,nearEnemySite=false;
            for(const auto& e:s->enemy_armies){double d=distance(position(e),front);if(d<7)contact=true;if(d<18)nearEnemySite=true;}
            for(const auto& kv:knownBuildings)if(distance(position(kv.second),front)<26)nearEnemySite=true;
            int readyForSite=0;for(const auto& a:s->armies)if(ranged(a.Sort)&&distance(position(a),front)<10&&a.Blood*2>a.MaxBlood)++readyForSite;
            bool siteReady=!nearEnemySite||readyForSite>=(frame>=36000?6:12);
            if(!contact&&siteReady&&close>=std::min(total,frame>=36000?8:10)&&priest&&distance(position(*priest),front)<(nearEnemySite?11:16)&&frame-advanceFrame>70){
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
            if(total<(frame>=36000?7:9)){
                // Regroup on ground already crossed when the advance loses its strength.
                double back=0;Pos previous=front;
                for(auto it=marchRoute.rbegin();it!=marchRoute.rend();++it){
                    back+=distance(previous,*it);previous=*it;
                    if(back>=10){front=*it;marchRoute.erase(it.base(),marchRoute.end());break;}
                }
                planFrame=-1000;transition(1);
            }
            int readyRanged=0,healthy=0;
            for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&distance(position(a),front)<9&&a.Blood*2>a.MaxBlood){++healthy;if(ranged(a.Sort))++readyRanged;}
            if(factoryKnown&&distance(front,destination)<10&&s->enemy_armies.size()<3&&
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
            if(priest&&priestGuard.count(a.SN))goal=position(*priest)-direction*1.5+Pos(-direction.y,direction.x)*((a.SN%3)-1)*1.5;
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
            double ownRange=a.Sort==AT_STONE_THROWER?10:a.Sort==AT_BOWMAN?5:a.Sort==AT_IMPROVED?6:rangeUnit?7:1;
            if(rangeUnit&&a.Sort!=AT_STONE_THROWER)ownRange+=(done(BUILDING_MARKET_WOOD_UPGRADE)?1:0)+(done(BUILDING_MARKET_CRAFT_UPGRADE)?1:0);
            for(const auto& e:s->enemy_armies){
                Pos ep=position(e);double d=attackDistance(ap,ep);
                bool huntsPriest=priest&&e.WorkObjectSN==priest->SN;
                double acquire=rangeUnit?ownRange+(huntsPriest?6:2):huntsPriest?18:phase>=1?12:10;
                bool cleanup=phase==0&&frame>=21000&&s->enemy_armies.size()<=3&&
                    troops(true,false)>=6&&!priestGuard.count(a.SN)&&distance(ep,base)<26;
                if(cleanup)acquire=40;
                if(e.Blood<=0||(!huntsPriest&&!cleanup&&distance(ep,front)>15)||d>acquire)continue;
                double value=d*2+(ranged(e.Sort)?-9:0);
                if(huntsPriest)value-=1000;
                if(e.Sort==AT_STONE_THROWER)value-=4;
                value+=e.Blood*0.01;
                if(value<best){best=value;target=&e;}
            }
            if(rangeUnit){
                if(frame<dodgeUntil[a.SN])continue;
                const tagArmy* artillery=nullptr;
                for(const auto& e:s->enemy_armies)if(e.Sort==AT_STONE_THROWER&&attackDistance(ap,position(e))<13){
                    const tagArmy* blastTarget=army(e.WorkObjectSN);
                    if(e.WorkObjectSN==a.SN||(blastTarget&&distance(ap,position(*blastTarget))<1.8)){artillery=&e;break;}
                }
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
            bool crosses=false;for(const auto& point:approach)if(attackDistance(point,position(kv.second))<10){crosses=true;break;}
            if(!crosses)continue;
            const tagArmy* tank=army(kv.second.Project);
            if(!tank||tank->Sort==AT_PRIEST||tank->MaxBlood<70||tank->Blood*2<tank->MaxBlood)return false;
        }
        return true;
    }
    Pos retreatPoint(const tagArmy& p) const {
        Pos origin=position(p);
        int sx=int(origin.x),sy=int(origin.y),start=sx*height+sy;
        if(!inBounds(sx,sy))return origin;
        std::vector<double> costs(width*height,1e20);
        using Entry=std::pair<double,int>;
        std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
        costs[start]=0;queue.push({0,start});
        Pos best=origin;double bestValue=1e20;
        const int dx[]={1,-1,0,0},dy[]={0,0,1,-1};
        auto exposure=[&](Pos point){
            double value=0;
            for(const auto& e:s->enemy_armies)if(e.Blood>0){
                double safe=ranged(e.Sort)?12:5;
                double weight=e.WorkObjectSN==p.SN?2.0:1.0;
                value+=std::max(0.0,safe-attackDistance(point,position(e)))*weight;
            }
            for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER)
                value+=std::max(0.0,11-attackDistance(point,position(kv.second)))*3;
            return value;
        };
        while(!queue.empty()){
            auto item=queue.top();queue.pop();int id=item.second,x=id/height,y=id%height;
            if(item.first>costs[id]+0.01)continue;
            Pos point(x+0.5,y+0.5);double d=distance(origin,point);
            if(d>=4){
                double value=exposure(point)*5+costs[id]*0.08+distance(point,priestAnchor())*0.1;
                if(value<bestValue){bestValue=value;best=point;}
            }
            for(int k=0;k<4;++k){int nx=x+dx[k],ny=y+dy[k];
                if(!open(nx,ny)||distance(origin,Pos(nx+0.5,ny+0.5))>9)continue;
                int h1=terrain[x][y].height,h2=terrain[nx][ny].height;
                if(h1>=0&&h2>=0&&h1!=h2)continue;
                int next=nx*height+ny;double cost=costs[id]+1+exposure(Pos(nx+0.5,ny+0.5))*2;
                if(cost<costs[next]){costs[next]=cost;queue.push({cost,next});}
            }
        }
        return best;
    }
    void priestControl() {
        if(!priest)return;
        const tagArmy& p=*priest;Pos pp=position(p);
        Pos anchor=priestAnchor();
        // A withdrawn priest must not walk back into a firing zone to reach the old rally point.
        bool unsafeAnchor=false;
        for(const auto& e:s->enemy_armies)if(e.Blood>0&&attackDistance(anchor,position(e))<(ranged(e.Sort)?12:5))unsafeAnchor=true;
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER&&attackDistance(anchor,position(kv.second))<11)unsafeAnchor=true;
        if(unsafeAnchor&&phase>=1){
            bool found=false;
            for(auto it=marchRoute.rbegin();it!=marchRoute.rend();++it){
                if(distance(pp,*it)>20)continue;
                bool safe=true;
                for(const auto& e:s->enemy_armies)if(e.Blood>0&&attackDistance(*it,position(e))<(ranged(e.Sort)?12:5))safe=false;
                for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER&&attackDistance(*it,position(kv.second))<11)safe=false;
                if(safe){anchor=*it;found=true;break;}
            }
            if(!found)anchor=retreatPoint(p);
        }
        const tagArmy* current=enemy(p.WorkObjectSN);
        double nearest=1e20;
        for(const auto& e:s->enemy_armies)if(e.Blood>0)nearest=std::min(nearest,attackDistance(pp,position(e)));
        for(const auto& e:s->enemy_armies)if(e.Sort==AT_STONE_THROWER&&attackDistance(pp,position(e))<14){
            const tagArmy* blastTarget=army(e.WorkObjectSN);
            if(e.WorkObjectSN==p.SN||(blastTarget&&distance(pp,position(*blastTarget))<1.8)){
                if(frame>=priestRetreatUntil){priestRetreat=retreatPoint(p);priestRetreatUntil=frame+80;}
                move(p,priestRetreat,18);return;
            }
        }
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_ARROWTOWER&&kv.second.Project==p.SN&&attackDistance(pp,position(kv.second))<11){
            if(p.WorkObjectSN==kv.first||p.Blood<60||phase!=3){
                priestRetreat=retreatPoint(p);priestRetreatUntil=frame+125;
                move(p,priestRetreat,15);return;
            }
        }
        int escorts=0;for(const auto& a:s->armies)if(a.Sort!=AT_PRIEST&&distance(pp,position(a))<8)++escorts;
        const tagArmy* hunter=nullptr;double hunterDistance=1e20;
        bool meleePressure=false;
        for(const auto& e:s->enemy_armies)if(e.Blood>0&&!ranged(e.Sort)&&distance(pp,position(e))<4.5){
            const tagArmy* shield=army(e.WorkObjectSN);
            if(e.WorkObjectSN==p.SN||(!shield&&distance(pp,position(e))<3)||
               (shield&&shield->Blood*2<shield->MaxBlood))meleePressure=true;
        }
        for(const auto& e:s->enemy_armies)if(e.Blood>0&&e.WorkObjectSN==p.SN){
            double d=attackDistance(pp,position(e));if(d<(ranged(e.Sort)?12:4.5)&&d<hunterDistance){hunter=&e;hunterDistance=d;}
        }
        bool activeConversion=current&&p.NowState==HUMAN_STATE_ATTACKING&&attackDistance(pp,position(*current))<11.8;
        if(hunter&&(!meleePressure||!ranged(hunter->Sort))&&p.Blood>=60&&p.ConvertCooldown==0&&hunterDistance<=11.3&&!activeConversion){
            action(p.SN,hunter->SN,p.WorkObjectSN,p.NowState,18);return;
        }
        bool holdingHunter=activeConversion&&hunter&&current->SN==hunter->SN&&p.Blood>=50;
        if((meleePressure&&!holdingHunter)||(hunter&&(!activeConversion||p.Blood<50||(!ranged(hunter->Sort)&&hunterDistance<2.2&&!holdingHunter)))){
            if(frame>=priestRetreatUntil||distance(pp,priestRetreat)<1.2){priestRetreat=retreatPoint(p);priestRetreatUntil=frame+125;}
            move(p,priestRetreat,18);return;
        }
        if(frame<priestRetreatUntil){move(p,priestRetreat,45);return;}
        // Preserve conversion once the target is valid and in range.
        if(current&&p.NowState==HUMAN_STATE_ATTACKING&&attackDistance(pp,position(*current))<11.8)return;
        for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE&&p.WorkObjectSN==kv.first&&p.NowState!=HUMAN_STATE_IDLE)return;
        if(phase==0&&frame<4000&&openingScoutStep<8&&!danger(pp,12)){
            double angle=std::atan2(direction.y,direction.x)+openingScoutStep*3.141592653589793/4;
            Pos scoutPoint=freePoint(base+Pos(std::cos(angle),std::sin(angle))*13,p.SN,false);
            bool safe=!danger(scoutPoint,12);
            for(const auto& r:s->resources)if(r.Blood>0&&(r.Type==RESOURCE_LION||r.Type==RESOURCE_ELEPHANT)&&distance(position(r),scoutPoint)<7)safe=false;
            if(openingScoutFrame==0)openingScoutFrame=frame;
            if(!safe||distance(pp,scoutPoint)<1.4||frame-openingScoutFrame>400){++openingScoutStep;openingScoutFrame=frame;}
            else {move(p,scoutPoint,60);return;}
        }
        if(phase>=1&&(escorts<4||distance(pp,anchor)>4.5)){move(p,anchor,40);return;}
        if(p.ConvertCooldown==0){
            const tagArmy* best=nullptr;double score=-1e20;
            for(const auto& e:s->enemy_armies){
                double d=attackDistance(pp,position(e));if(e.Blood<=0||d>11.3||distance(position(e),front)>15)continue;
                if(phase==0&&!ranged(e.Sort)){
                    bool towerLock=false;for(const auto& b:s->buildings)if(b.Type==BUILDING_ARROWTOWER&&e.WorkObjectSN==b.SN)towerLock=true;
                    if(!towerLock&&d>5)continue;
                }
                double v=e.Blood-d*2;
                if(e.Sort==AT_HOPLITE||e.Sort==AT_CAVALRY)v+=70;
                if(ranged(e.Sort)&&phase==0)v+=140;
                if(e.WorkObjectSN==p.SN)v+=300;
                if(v>score){score=v;best=&e;}
            }
            if(best&&!meleePressure){action(p.SN,best->SN,p.WorkObjectSN,p.NowState,20);return;}
        }
        if(phase==3&&escorts>=6){
            for(const auto& kv:knownBuildings)if(kv.second.Type==BUILDING_SIEGE){
                if((s->enemy_armies.empty()||nearest>11)&&safeConversionRoute(kv.second)){action(p.SN,kv.first,p.WorkObjectSN,p.NowState,35);return;}
            }
        }
        const tagArmy* healing=army(p.WorkObjectSN);
        const tagArmy* approachTarget=army(healApproachSN);
        if(approachTarget&&approachTarget->Blood<approachTarget->MaxBlood&&frame<healApproachUntil&&nearest>11){
            if(attackDistance(pp,position(*approachTarget))<=1.05){
                action(p.SN,approachTarget->SN,p.WorkObjectSN,p.NowState,30);healApproachSN=-1;healFrame=frame;
            }else move(p,healApproach,30);
            return;
        }
        healApproachSN=-1;
        if(healing&&healing->Blood<healing->MaxBlood){
            if(healSN!=healing->SN||healBlood!=healing->Blood){healSN=healing->SN;healBlood=healing->Blood;healFrame=frame;}
            if(frame-healFrame>200&&nearest>11&&distance(position(*healing),anchor)<7&&available(p.SN,60)){
                Pos target=position(*healing),best;double score=1e20;
                for(int dx=-1;dx<=1;++dx)for(int dy=-1;dy<=1;++dy){
                    int x=int(target.x)+dx,y=int(target.y)+dy;Pos point(x+0.5,y+0.5);
                    if(!open(x,y)||attackDistance(point,target)>1.05||distance(point,target)<0.6)continue;
                    bool occupiedByAlly=false;
                    for(const auto& ally:s->armies)if(ally.SN!=p.SN&&distance(position(ally),point)<0.9)occupiedByAlly=true;
                    if(occupiedByAlly||danger(point,11))continue;
                    double value=distance(pp,point);if(value<score){score=value;best=point;}
                }
                if(score<1e20&&distance(pp,best)>0.65){
                    healApproachSN=healing->SN;healApproachUntil=frame+300;healApproach=best;
                    move(p,best,30);
                }else remember(p.SN,2,healing->SN,Pos(),api->HumanAction(p.SN,healing->SN));
                healFrame=frame;return;
            }
        }else healSN=-1;
        if(healing&&healing->Blood<healing->MaxBlood&&distance(position(*healing),anchor)<7&&attackDistance(pp,position(*healing))<1.2&&nearest>11)return;
        if(nearest>9){
            const tagArmy* injured=nullptr;double missing=0;
            for(const auto& ally:s->armies)if(ally.Sort!=AT_PRIEST&&distance(position(ally),anchor)<6&&distance(pp,position(ally))<3&&ally.Blood<ally.MaxBlood){
                double need=double(ally.MaxBlood-ally.Blood)/ally.MaxBlood;if(need>missing){missing=need;injured=&ally;}
            }
            if(injured){action(p.SN,injured->SN,p.WorkObjectSN,p.NowState,30);return;}
        }
        if(distance(pp,anchor)>1.6||p.NowState==HUMAN_STATE_ATTACKING||p.NowState==HUMAN_STATE_WORKING)move(p,anchor,45);
    }
public:
    void tick(AI& a,const tagInfo& snapshot) {
        if(!snapshot.theMap||snapshot.buildings.empty()||snapshot.GameFrame==last)return;
        if(snapshot.GameFrame<last)*this=Strategy();
        s=&snapshot;api=&a;frame=s->GameFrame;
        if(last>=0&&frame-last<6)return;
        last=frame;sent.clear();center=nullptr;priest=nullptr;
        for(const auto& b:s->buildings)if(b.Type==BUILDING_CENTER)center=&b;
        for(const auto& u:s->armies)if(u.Sort==AT_PRIEST)priest=&u;
        if(!center||!priest)return;
        base=position(*center);
        if(length(direction)<0.1)direction=unit(Pos(s->theMap->size()*0.5,(*s->theMap)[0].size()*0.5)-base);
        for(const auto& b:s->enemy_buildings)if(b.Blood>0)knownBuildings[b.SN]=b;
        for(auto it=knownBuildings.begin();it!=knownBuildings.end();){
            bool exists=false;for(const auto& b:s->enemy_buildings)if(b.SN==it->first&&b.Blood>0)exists=true;
            if(!exists)it=knownBuildings.erase(it);else ++it;
        }
        wood=s->Wood;food=s->Meat;gold=s->Gold;freePop=std::max(0,int(s->Human_MaxNum-s->Human_Num));
        receipt();grid();
        towers();campaign();priestControl();scouting();
        production();economy();construction();
    }
};
}

void UsrAI::processData()
{
    tagInfo snapshot=getInfo();
    static newaoe_ai::Strategy strategy;
    strategy.tick(*this,snapshot);
    for(auto& command:InsPerFrame){
        if(!command.self){
            // Recover only a missing command actor that still exists. Engine validation remains in charge.
            const auto actor=g_Object.find(command.SN);
            if(actor!=g_Object.end()&&actor->second)command.self=actor->second;
        }
    }
}
