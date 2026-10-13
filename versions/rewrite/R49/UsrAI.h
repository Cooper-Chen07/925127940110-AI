#ifndef USRAI_H
#define USRAI_H
#include "ai.h"
extern tagGame tagUsrGame;
extern ins UsrIns;

class UsrAI : public AI
{
public:
    UsrAI() { this->id = 0; }
    ~UsrAI() {}
private:
    void processData() override;
    tagInfo getInfo() { return tagUsrGame.getInfo(); }
    int AddToIns(instruction command) override
    {
        QMutexLocker locker(&UsrIns.lock);
        command.id = UsrIns.g_id++;
        UsrIns.instructions.push(command);
        return command.id;
    }
    void clearInsRet() override { tagUsrGame.clearInsRet(); }
};
#endif
