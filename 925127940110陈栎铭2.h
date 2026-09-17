#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>
#include <set>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

// ============================================================
// 【3.0.7g·关键修正】类里**不放任何数据成员**（与课设模板保持一致）
//   模板末尾写着 "DO NOT MODIFY THE CODE IN THE CLASS"，且模板类是零成员的。
//   若评测系统是把本文件链接到**预先编译好的引擎**，引擎里 `new UsrAI()` 的 sizeof
//   是按模板类算的（无成员）→ 我们一旦在类里塞 40KB 成员（m_map[100][100] 等），
//   对象就会写到分配块外面 → 开局就踩坏堆、评测机 SIGSEGV
//   （本地是整个工程一起编译，sizeof 正确，所以看不出来）。
//   因此所有跨帧状态改为 UsrAI.cpp 里的**文件级全局变量**（与原来的成员同名），
//   类里只保留方法声明 —— 这样 sizeof(UsrAI) == sizeof(AI)，与模板完全一致。
// ============================================================
class UsrAI:public AI
{
public:
    UsrAI(){this->id=0;}
    ~UsrAI(){}

private:
    void processData() override;
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id=UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/

    // ===== 祭司探路 =====
    void scoutWithPriest(const tagInfo& info);       // 祭司探路（前期，视野12）
    void scoutWithScout(const tagInfo& info);        // 侦察骑兵探路（后期，视野8速度快的远探）

    // ===== 地图 =====
    void updateMap(const tagInfo& info);             // 建立地图标记数组
    void markBlock(int bx, int by, int size, int val);// 标记一片占用区域
    bool findBuildBlock(const tagInfo& info, int& x, int& y, int w, int h, int nearX = -1, int nearY = -1);
    bool canPlace(const tagInfo& info, int i, int j, int w, int h) const;
    bool findBuildBlockNear(const tagInfo& info, int& x, int& y, int w, int h, int cx, int cy, int maxR);

    // ===== 经济：农民工作分配 =====
    void manageVillagers(const tagInfo& info);
    int  findNearestResource(const tagInfo& info, int type, int farmerSN);
    int  findNearestHunt(const tagInfo& info, int farmerSN);
    int  findNearestTree(const tagInfo& info, int farmerSN);
    int  findNearestFarm(const tagInfo& info, int farmerSN);
    bool isBadTarget(int sn, int frame) const;       // 该目标近期是否被判过"卡住/不可达"
    int  countBuilding(const tagInfo& info, int type) const;
    bool mapFoodLeft(const tagInfo& info) const;     // 地图上还有浆果/猎物？（决定要不要开农田）
    int  countArmy(const tagInfo& info, int sort) const;

    // ===== 生产与科技 =====
    void manageCenter(const tagInfo& info);
    void researchTech(const tagInfo& info);
    bool canUpgradeBronze(const tagInfo& info) const;
    void buildBuildings(const tagInfo& info);
    void buildResourceDepots(const tagInfo& info);
    void trainArmy(const tagInfo& info);

    // ===== 防守 =====
    void defense(const tagInfo& info);
    void handlePriest(const tagInfo& info);
    bool movePriest(int priestSN, double px, double py, double gx, double gy, int frame);
    bool isStaticBlock(int bx, int by) const;
    void adjustReachableTarget(double& gx, double& gy) const;
    void buildArrowTower(const tagInfo& info);
    void getPriestHome(const tagInfo& info, int& hx, int& hy) const;
};

/*##########YOUR CODE BEGINS HERE##########*/




/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H
