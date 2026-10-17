/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  port_menu.h
 * @brief Port settings menu, opened with L + R + Start.
 */

#ifndef SOLOADER_PORT_MENU_H
#define SOLOADER_PORT_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

// 1 when L + R + Start are held
int port_menu_check();

// Shows the menu until it is closed (call right after a buffer swap)
void port_menu_run();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_PORT_MENU_H
