#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/*lcd init*/
#define LCD_WIDTH      400
#define LCD_HEIGHT     300

#define RLCD_DC_PIN    GPIO_NUM_5  
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_RST_PIN   GPIO_NUM_41
#define RLCD_TE_PIN    GPIO_NUM_6

/*version - auto updated by CI*/
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

/*DeepSeek API*/
#ifndef DEEPSEEK_API_KEY
#define DEEPSEEK_API_KEY ""
#endif

#endif