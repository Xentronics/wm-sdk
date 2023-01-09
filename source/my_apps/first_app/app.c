/* Copyright 2017 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

/*
 * \file    app.c
 * \brief   This file is a minimal app to start the stack
 */

/* Includes ******************************************************************************/
#include <stdlib.h>

#include "api.h"
#include "node_configuration.h"
/* Defines and Typedefs ******************************************************************/
/* Pins used for LEDs */
#define LED_1_PIN         NRF_GPIO_PIN_MAP(0,13) /* P0.13 */
#define LED_2_PIN         NRF_GPIO_PIN_MAP(0,14) /* P0.14 */
#define LED_3_PIN         NRF_GPIO_PIN_MAP(0,15) /* P0.15 */
#define LED_4_PIN         NRF_GPIO_PIN_MAP(0,16) /* P0.16 */


/**
 * \brief   Initialization callback for application
 *
 * This function is called after hardware has been initialized but the
 * stack is not yet running.
 *
 */
void App_init(const app_global_functions_t * functions)
{
    // Basic configuration of the node with a unique node address
    if (configureNodeFromBuildParameters() != APP_RES_OK)
    {
        // Could not configure the node
        // It should not happen except if one of the config value is invalid
        return;
    }

      // Set Leds as an outputs
      nrf_gpio_cfg_output(LED_1_PIN);
      nrf_gpio_cfg_output(LED_2_PIN);
      nrf_gpio_cfg_output(LED_3_PIN);
      nrf_gpio_cfg_output(LED_4_PIN);

      nrf_gpio_pin_clear(LED_1_PIN);    // Logic 0, Turn ON LED
      nrf_delay_ms(5000);
      nrf_gpio_pin_set(LED_1_PIN);    // Logic 0, Turn ON LED
    /*
     * Start the stack.
     * This is really important step, otherwise the stack will stay stopped and
     * will not be part of any network. So the device will not be reachable
     * without reflashing it
     */
    lib_state->startStack();
}

