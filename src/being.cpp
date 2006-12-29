/*
 *  The Mana World Server
 *  Copyright 2004 The Mana World Development Team
 *
 *  This file is part of The Mana World.
 *
 *  The Mana World  is free software; you can redistribute  it and/or modify it
 *  under the terms of the GNU General  Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or any later version.
 *
 *  The Mana  World is  distributed in  the hope  that it  will be  useful, but
 *  WITHOUT ANY WARRANTY; without even  the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 *  more details.
 *
 *  You should  have received a  copy of the  GNU General Public  License along
 *  with The Mana  World; if not, write to the  Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *  $Id$
 */

#include "being.h"

#include "controller.h"

#include "utils/logger.h"

void Being::update()
{
    if (mController)
        mController->update();

    mHitsTaken.clear();
}

void Being::damage(Damage damage)
{
    int HPloss;

    HPloss = damage; // TODO: Implement complex damage calculation here

    mHitpoints -= HPloss;
    mHitsTaken.push_back(HPloss);
    LOG_DEBUG("Being " << getPublicID() << " got hit", 0);
}

void Being::performAttack(MapComposite* map)
{
    //Monster attack
}
