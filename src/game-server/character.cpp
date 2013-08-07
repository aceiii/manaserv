/*
 *  The Mana Server
 *  Copyright (C) 2004-2010  The Mana World Development Team
 *
 *  This file is part of The Mana Server.
 *
 *  The Mana Server is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  any later version.
 *
 *  The Mana Server is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Mana Server.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "game-server/character.h"

#include "common/configuration.h"
#include "game-server/accountconnection.h"
#include "game-server/attributemanager.h"
#include "game-server/buysell.h"
#include "game-server/inventory.h"
#include "game-server/item.h"
#include "game-server/itemmanager.h"
#include "game-server/gamehandler.h"
#include "game-server/map.h"
#include "game-server/mapcomposite.h"
#include "game-server/mapmanager.h"
#include "game-server/state.h"
#include "game-server/trade.h"
#include "scripting/scriptmanager.h"
#include "net/messagein.h"
#include "net/messageout.h"
#include "serialize/characterdata.h"

#include "utils/logger.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits.h>

Script::Ref CharacterComponent::mDeathCallback;
Script::Ref CharacterComponent::mDeathAcceptedCallback;
Script::Ref CharacterComponent::mLoginCallback;

static bool executeCallback(Script::Ref function, Entity &entity)
{
    if (!function.isValid())
        return false;

    Script *script = ScriptManager::currentState();
    script->prepare(function);
    script->push(&entity);
    script->execute(entity.getMap());
    return true;
}


CharacterComponent::CharacterComponent(Entity &entity, MessageIn &msg):
    mClient(nullptr),
    mConnected(true),
    mTransactionHandler(nullptr),
    mDatabaseID(-1),
    mHairStyle(0),
    mHairColor(0),
    mSendAttributePointsStatus(false),
    mAttributePoints(0),
    mCorrectionPoints(0),
    mSendAbilityCooldown(false),
    mParty(0),
    mTransaction(TRANS_NONE),
    mTalkNpcId(0),
    mNpcThread(0),
    mBaseEntity(&entity)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();

    auto &attributeScope = attributeManager->getAttributeScope(CharacterScope);
    LOG_DEBUG("Character creation: initialisation of "
              << attributeScope.size() << " attributes.");
    for (auto &attributeIt : attributeScope)
        beingComponent->createAttribute(attributeIt.first, attributeIt.second);

    auto *actorComponent = entity.getComponent<ActorComponent>();
    actorComponent->setWalkMask(Map::BLOCKMASK_WALL);
    actorComponent->setBlockType(BLOCKTYPE_CHARACTER);
    actorComponent->setSize(16);


    auto *abilityComponent = new AbilityComponent();
    entity.addComponent(abilityComponent);
    abilityComponent->signal_ability_changed.connect(
            sigc::mem_fun(this, &CharacterComponent::abilityStatusChanged));
    abilityComponent->signal_global_cooldown_activated.connect(
            sigc::mem_fun(this,
                          &CharacterComponent::abilityCooldownActivated));

    // Get character data.
    mDatabaseID = msg.readInt32();
    beingComponent->setName(msg.readString());

    CharacterData characterData(&entity, this);
    deserializeCharacterData(characterData, msg);

    Inventory(&entity, mPossessions).initialize();
    modifiedAllAttributes(entity);;

    beingComponent->signal_attribute_changed.connect(sigc::mem_fun(
            this, &CharacterComponent::attributeChanged));

    for (auto &abilityIt : abilityComponent->getAbilities())
        mModifiedAbilities.insert(abilityIt.first);
}

CharacterComponent::~CharacterComponent()
{
    delete mNpcThread;
}

void CharacterComponent::update(Entity &entity)
{
    // Dead character: don't regenerate anything else
    if (entity.getComponent<BeingComponent>()->getAction() == DEAD)
        return;

    if (!mModifiedAbilities.empty())
        sendAbilityUpdate(entity);

    if (mSendAbilityCooldown)
        sendAbilityCooldownUpdate(entity);

    if (mSendAttributePointsStatus)
        sendAttributePointsStatus(entity);
}

void CharacterComponent::characterDied(Entity *being)
{
    executeCallback(mDeathCallback, *being);
}

void CharacterComponent::respawn(Entity &entity)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();

    if (beingComponent->getAction() != DEAD)
    {
        LOG_WARN("Character \"" << beingComponent->getName()
                 << "\" tried to respawn without being dead");
        return;
    }

    // Make it alive again
    beingComponent->setAction(entity, STAND);

    // Execute respawn callback when set
    if (executeCallback(mDeathAcceptedCallback, entity))
        return;

    // No script respawn callback set - fall back to hardcoded logic
    const double maxHp = beingComponent->getModifiedAttribute(ATTR_MAX_HP);
    beingComponent->setAttribute(entity, ATTR_HP, maxHp);
    // Warp back to spawn point.
    int spawnMap = Configuration::getValue("char_respawnMap", 1);
    int spawnX = Configuration::getValue("char_respawnX", 1024);
    int spawnY = Configuration::getValue("char_respawnY", 1024);

    GameState::enqueueWarp(&entity, MapManager::getMap(spawnMap),
                           Point(spawnX, spawnY));
}

void CharacterComponent::abilityStatusChanged(int id)
{
    mModifiedAbilities.insert(id);
}

void CharacterComponent::abilityCooldownActivated()
{
    mSendAbilityCooldown = true;
}

void CharacterComponent::sendAbilityUpdate(Entity &entity)
{
    auto &abilities = entity.getComponent<AbilityComponent>()->getAbilities();

    MessageOut msg(GPMSG_ABILITY_STATUS);
    for (unsigned id : mModifiedAbilities)
    {
        auto it = abilities.find(id);
        if (it == abilities.end())
            continue; // got deleted

        msg.writeInt8(id);
        msg.writeInt32(it->second.rechargeTimeout.remaining());
    }

    mModifiedAbilities.clear();
    gameHandler->sendTo(mClient, msg);
}

void CharacterComponent::sendAbilityCooldownUpdate(Entity &entity)
{
    MessageOut msg(GPMSG_ABILITY_COOLDOWN);
    auto *abilityComponent = entity.getComponent<AbilityComponent>();
    msg.writeInt16(abilityComponent->globalCooldown());
    gameHandler->sendTo(mClient, msg);
    mSendAbilityCooldown = false;
}

void CharacterComponent::sendAttributePointsStatus(Entity &entity)
{
    MessageOut msg(GPMSG_ATTRIBUTE_POINTS_STATUS);
    msg.writeInt16(mAttributePoints);
    msg.writeInt16(mCorrectionPoints);
    gameHandler->sendTo(mClient, msg);
    mSendAttributePointsStatus = false;
}

void CharacterComponent::cancelTransaction()
{
    TransactionType t = mTransaction;
    mTransaction = TRANS_NONE;
    switch (t)
    {
        case TRANS_TRADE:
            static_cast< Trade * >(mTransactionHandler)->cancel();
            break;
        case TRANS_BUYSELL:
            static_cast< BuySell * >(mTransactionHandler)->cancel();
            break;
        case TRANS_NONE:
            return;
    }
}

Trade *CharacterComponent::getTrading() const
{
    return mTransaction == TRANS_TRADE
        ? static_cast< Trade * >(mTransactionHandler) : nullptr;
}

BuySell *CharacterComponent::getBuySell() const
{
    return mTransaction == TRANS_BUYSELL
        ? static_cast< BuySell * >(mTransactionHandler) : nullptr;
}

void CharacterComponent::setTrading(Trade *t)
{
    if (t)
    {
        cancelTransaction();
        mTransactionHandler = t;
        mTransaction = TRANS_TRADE;
    }
    else
    {
        assert(mTransaction == TRANS_NONE || mTransaction == TRANS_TRADE);
        mTransaction = TRANS_NONE;
    }
}

void CharacterComponent::setBuySell(BuySell *t)
{
    if (t)
    {
        cancelTransaction();
        mTransactionHandler = t;
        mTransaction = TRANS_BUYSELL;
    }
    else
    {
        assert(mTransaction == TRANS_NONE || mTransaction == TRANS_BUYSELL);
        mTransaction = TRANS_NONE;
    }
}

void CharacterComponent::sendStatus(Entity &entity)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();
    MessageOut attribMsg(GPMSG_PLAYER_ATTRIBUTE_CHANGE);
    for (std::set<size_t>::const_iterator i = mModifiedAttributes.begin(),
         i_end = mModifiedAttributes.end(); i != i_end; ++i)
    {
        int attr = *i;
        attribMsg.writeInt16(attr);
        attribMsg.writeInt32(beingComponent->getAttributeBase(attr) * 256);
        attribMsg.writeInt32(beingComponent->getModifiedAttribute(attr) * 256);
    }
    if (attribMsg.getLength() > 2) gameHandler->sendTo(mClient, attribMsg);
    mModifiedAttributes.clear();
}

void CharacterComponent::modifiedAllAttributes(Entity &entity)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();

    LOG_DEBUG("Marking all attributes as changed, requiring recalculation.");
    for (auto attribute : beingComponent->getAttributes())
    {
        beingComponent->recalculateBaseAttribute(entity, attribute.first);
        mModifiedAttributes.insert(attribute.first);
    }
}

void CharacterComponent::attributeChanged(Entity *entity, unsigned attr)
{
    auto *beingComponent = entity->getComponent<BeingComponent>();

    // Inform the client of this attribute modification.
    accountHandler->updateAttributes(getDatabaseID(), attr,
                                   beingComponent->getAttributeBase(attr),
                                   beingComponent->getModifiedAttribute(attr));
    mModifiedAttributes.insert(attr);
}

void CharacterComponent::incrementKillCount(int monsterType)
{
    std::map<int, int>::iterator i = mKillCount.find(monsterType);
    if (i == mKillCount.end())
    {
        // Character has never murdered this species before
        mKillCount[monsterType] = 1;
    }
    else
    {
        // Character is a repeated offender
        mKillCount[monsterType] ++;
    }
}

int CharacterComponent::getKillCount(int monsterType) const
{
    std::map<int, int>::const_iterator i = mKillCount.find(monsterType);
    if (i != mKillCount.end())
        return i->second;
    return 0;
}

AttribmodResponseCode CharacterComponent::useCharacterPoint(Entity &entity,
                                                            int attribute)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();

    if (!attributeManager->isAttributeDirectlyModifiable(attribute))
        return ATTRIBMOD_INVALID_ATTRIBUTE;
    if (!mAttributePoints)
        return ATTRIBMOD_NO_POINTS_LEFT;

    setAttributePoints(mAttributePoints - 1);

    const double base = beingComponent->getAttributeBase(attribute);
    beingComponent->setAttribute(entity, attribute, base + 1);
    beingComponent->updateDerivedAttributes(entity, attribute);
    return ATTRIBMOD_OK;
}

AttribmodResponseCode CharacterComponent::useCorrectionPoint(Entity &entity,
                                                             int attribute)
{
    auto *beingComponent = entity.getComponent<BeingComponent>();

    if (!attributeManager->isAttributeDirectlyModifiable(attribute))
        return ATTRIBMOD_INVALID_ATTRIBUTE;
    if (!mCorrectionPoints)
        return ATTRIBMOD_NO_POINTS_LEFT;
    if (beingComponent->getAttributeBase(attribute) <= 1)
        return ATTRIBMOD_DENIED;

    setCorrectionPoints(mCorrectionPoints - 1);
    setAttributePoints(mAttributePoints + 1);

    const double base = beingComponent->getAttributeBase(attribute);
    beingComponent->setAttribute(entity, attribute, base - 1);
    return ATTRIBMOD_OK;
}

void CharacterComponent::startNpcThread(Script::Thread *thread, int npcId)
{
    if (mNpcThread)
        delete mNpcThread;

    mNpcThread = thread;
    mTalkNpcId = npcId;

    resumeNpcThread();
}

void CharacterComponent::resumeNpcThread()
{
    Script *script = ScriptManager::currentState();

    assert(script->getCurrentThread() == mNpcThread);

    if (script->resume())
    {
        MessageOut msg(GPMSG_NPC_CLOSE);
        msg.writeInt16(mTalkNpcId);
        gameHandler->sendTo(mClient, msg);

        mTalkNpcId = 0;
        mNpcThread = 0;
    }
}

void CharacterComponent::disconnected(Entity &entity)
{
    mConnected = false;

    // Make the dead characters respawn, even in case of disconnection.
    if (entity.getComponent<BeingComponent>()->getAction() == DEAD)
        respawn(entity);
    else
        GameState::remove(&entity);

    signal_disconnected.emit(entity);
}
void CharacterComponent::triggerLoginCallback(Entity &entity)
{
    executeCallback(mLoginCallback, entity);
}
