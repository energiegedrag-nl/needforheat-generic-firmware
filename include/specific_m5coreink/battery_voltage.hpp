#pragma once

#include <util/battery_monitor.hpp>

namespace M5CoreInkSpecific
{
    /**
     * Battery voltage task.
     *
     * Measures the battery voltage and adds it to the secure upload queue (battery_voltage__V), together with the
     * estimated state of charge (battery_soc__0) and, on battery, the estimated hours left (battery_runtime__h).
     */
    void BatteryVoltageTask(void *taskInfo);

    /**
     * Measure the resting voltage at boot, before Wi-Fi is switched on (call once, early in Initialize).
     * On battery the CoreInk switches off between rounds, so this runs every round with the radio still off.
     */
    void BatteryAtBoot();

    /** The latest battery reading (see BatteryMonitor::Reading); false before the first measurement. */
    bool GetBatteryReading(BatteryMonitor::Reading &reading);

    /**
     * Expected energy use per hour on battery (J/h), for the estimate of the hours left. Default 18 J/h: six rounds
     * per hour of about 3 J (boot and setup 1.25 J from the scheduler's model, plus measuring and uploading).
     * The firmware corrects it while running on battery (BatteryMonitor::Update).
     */
    void SetBatteryUsePerHour(double joulesPerHour);
} // namespace M5CoreInkSpecific
