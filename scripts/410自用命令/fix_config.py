#!/usr/bin/env python3
import re
import sys

ALL_DEVICES = [
    'ufi001c', 'ufi001b', 'ufi103s', 'qrzl903', 'w001', 'ufi003',
    'uz801', 'mf32', 'mf601', 'wf2', 'jz02v10', 'sp970v11', 'sp970v10'
]

def main(target_dev):
    input_file = '.config'
    output_file = f'{target_dev}.config'

    with open(input_file, 'r') as f:
        lines = f.readlines()

    final_lines = []

    for ln in lines:
        stripped = ln.strip()

        # 1. DEVICE 行
        m_dev = re.match(r'(# )?CONFIG_TARGET_msm89xx_msm8916_DEVICE_openstick-([a-zA-Z0-9]+)(.*)', stripped)
        if m_dev:
            dev = m_dev.group(2)
            if dev == target_dev:
                final_lines.append(f'CONFIG_TARGET_msm89xx_msm8916_DEVICE_openstick-{dev}=y\n')
            else:
                final_lines.append(f'# CONFIG_TARGET_msm89xx_msm8916_DEVICE_openstick-{dev} is not set\n')
            continue

        # 2. PROFILE
        if 'CONFIG_TARGET_PROFILE=' in ln:
            final_lines.append(f'CONFIG_TARGET_PROFILE="DEVICE_openstick-{target_dev}"\n')
            continue

        # 3. PACKAGE / DEFAULT 行（只处理 jz02v10）
        if ('CONFIG_PACKAGE_qcom-msm8916' in ln or 'CONFIG_DEFAULT_qcom-msm8916' in ln) and 'openstick-jz02v10' in ln:
            new_ln = ln.replace('openstick-jz02v10', f'openstick-{target_dev}')
            new_ln = re.sub(r'# CONFIG_', 'CONFIG_', new_ln)
            new_ln = re.sub(r' is not set', '=y', new_ln)
            final_lines.append(new_ln)
            continue

        # 4. 其他 PACKAGE / DEFAULT 行（包含 openstick-设备名）
        if ('CONFIG_PACKAGE_qcom-msm8916' in ln or 'CONFIG_DEFAULT_qcom-msm8916' in ln) and 'openstick-' in ln:
            # 如果这行包含目标设备，但它是 is not set，说明是原文里的，直接跳过
            if f'openstick-{target_dev}' in ln and 'is not set' in ln:
                continue
            # 其他设备的，保持原样（已经是 is not set）
            # 但如果是目标设备且是 =y，也保持原样（原文可能有）
            # 简单处理：原样保留
            final_lines.append(ln)
            continue

        # 其余原样保留
        final_lines.append(ln)

    with open(output_file, 'w') as f:
        f.writelines(final_lines)

    print(f'完成: {target_dev} -> {output_file}')

if __name__ == '__main__':
    if len(sys.argv) == 2:
        main(sys.argv[1])
    else:
        print(f'识别到型号: {ALL_DEVICES}')
        for dev in ALL_DEVICES:
            print(f'\n==== 处理: {dev} ====')
            main(dev)
