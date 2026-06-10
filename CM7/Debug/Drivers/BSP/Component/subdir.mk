################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
D:/stm32_workspace/ETH_FTP_SDFatFs_260610/H755_FTP_SD_DualCore/Drivers/BSP/Components/lan8742/lan8742.c 

OBJS += \
./Drivers/BSP/Component/lan8742.o 

C_DEPS += \
./Drivers/BSP/Component/lan8742.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/BSP/Component/lan8742.o: D:/stm32_workspace/ETH_FTP_SDFatFs_260610/H755_FTP_SD_DualCore/Drivers/BSP/Components/lan8742/lan8742.c Drivers/BSP/Component/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m7 -std=gnu11 -g3 -DDEBUG -DCORE_CM7 -DUSE_HAL_DRIVER -DSTM32H755xx -DUSE_PWR_DIRECT_SMPS_SUPPLY -c -I../Core/Inc -I../FATFS/Target -I../FATFS/App -I../../Drivers/STM32H7xx_HAL_Driver/Inc -I../../Drivers/STM32H7xx_HAL_Driver/Inc/Legacy -I../../Middlewares/Third_Party/FatFs/src -I../../Drivers/BSP/STM32H7xx_Nucleo -I../../Drivers/CMSIS/Device/ST/STM32H7xx/Include -I../../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-BSP-2f-Component

clean-Drivers-2f-BSP-2f-Component:
	-$(RM) ./Drivers/BSP/Component/lan8742.cyclo ./Drivers/BSP/Component/lan8742.d ./Drivers/BSP/Component/lan8742.o ./Drivers/BSP/Component/lan8742.su

.PHONY: clean-Drivers-2f-BSP-2f-Component

