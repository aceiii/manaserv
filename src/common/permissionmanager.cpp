/*
 *  The Mana Server
 *  Copyright (C) 2010  The Mana World Development Team
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

#include "common/permissionmanager.hpp"

#include "game-server/character.hpp"
#include "game-server/resourcemanager.hpp"
#include "utils/logger.h"
#include "utils/xml.hpp"


static std::map<std::string, unsigned char> permissions;
static std::string permissionFile;

void addPermission(std::string permission, char mask)
{

    std::map<std::string, unsigned char>::iterator i = permissions.find(permission);
    if (i == permissions.end())
    {
        permissions.insert(std::make_pair<std::string, unsigned char>(permission, mask));
    } else {
        i->second |= mask;
    }
}

void PermissionManager::initialize(const std::string & file)
{
    permissionFile = file;
    reload();
}

void PermissionManager::reload()
{
    int size;
    char *data = ResourceManager::loadFile(permissionFile, size);

    if (!data) {
        LOG_ERROR("Permission Manager: Could not find "
                  << permissionFile << "!");
        free(data);
        return;
    }

    xmlDocPtr doc = xmlParseMemory(data, size);
    free(data);

    if (!doc)
    {
        LOG_ERROR("Permission Manager: Error while parsing permission database ("
                  << permissionFile << ")!");
        return;
    }

    xmlNodePtr node = xmlDocGetRootElement(doc);
    if (!node || !xmlStrEqual(node->name, BAD_CAST "permissions"))
    {
        LOG_ERROR("Permission Manager: " << permissionFile
                  << " is not a valid database file!");
        xmlFreeDoc(doc);
        return;
    }

    LOG_INFO("Loading permission reference...");
    for (node = node->xmlChildrenNode; node != NULL; node = node->next)
    {
        unsigned char classmask = 0x01;
        if (!xmlStrEqual(node->name, BAD_CAST "class"))
        {
            continue;
        }
        int level = XML::getProperty(node, "level", 0);
        if (level < 1 || level > 8)
        {
            LOG_WARN("PermissionManager: Illegal class level "
                    <<level
                    <<" in "
                    <<permissionFile
                    <<" (allowed range: 1..8)");
            continue;
        }
        classmask = classmask << (level-1);


        xmlNodePtr perNode;
        for (perNode = node->xmlChildrenNode; perNode != NULL; perNode = perNode->next)
        {
            if (xmlStrEqual(perNode->name, BAD_CAST "allow"))
            {
                const char* permission = (const char*)perNode->xmlChildrenNode->content;
                if (permission && strlen(permission) > 0)
                {
                    addPermission(permission, classmask);
                }
            } else if (xmlStrEqual(perNode->name, BAD_CAST "deny")){
                const char* permission = (const char*)perNode->xmlChildrenNode->content;
                // To be implemented
            } else if (xmlStrEqual(perNode->name, BAD_CAST "alias")){
                const char* alias = (const char*)perNode->xmlChildrenNode->content;
                // To be implemented
            }
        }
    }

    LOG_INFO("Permission List:");
    for (std::map<std::string, unsigned char>::iterator i = permissions.begin();
         i != permissions.end();
         i++)
     {
         LOG_INFO(i->first<<" "<<(int)i->second);
     }
}


PermissionManager::Result PermissionManager::checkPermission(const Character* character, std::string permission)
{
    std::map<std::string, unsigned char>::iterator iP = permissions.find(permission);

    if (iP == permissions.end())
    {
        LOG_WARN("PermissionManager: Check for unknown permission \""<<permission<<"\" requested.");
        return PMR_UNKNOWN;
    }
    if (character->getAccountLevel() & iP->second)
    {
        return PMR_ALLOWED;
    } else {
        return PMR_DENIED;
    }
}
