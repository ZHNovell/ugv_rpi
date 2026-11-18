#!/usr/bin/env python3
"""
Скрипт для исправления PROJECT.md:
- Закрывает код-блоки (добавляет ``` после команд)
- Исправляет таблицы
"""

import re

with open('PROJECT.md', 'r') as f:
    lines = f.readlines()

output = []
in_code_block = False
code_block_lang = None

# Паттерны для определения начала код-блока
code_start_patterns = [
    (r'^\s*```bash\s*$', 'bash'),
    (r'^\s*```python\s*$', 'python'),
    (r'^\s*```javascript\s*$', 'javascript'),
    (r'^\s*```html\s*$', 'html'),
    (r'^\s*```css\s*$', 'css'),
    (r'^\s*```ini\s*$', 'ini'),
    (r'^\s*```yaml\s*$', 'yaml'),
    (r'^\s*```text\s*$', 'text'),
    (r'^\s*```\s*$', 'plain'),
]

# Команды, которые начинают bash-блок
bash_start_patterns = [
    r'^\s*# На Ubuntu',
    r'^\s*# На Orange Pi',
    r'^\s*sudo apt',
    r'^\s*cd ~/',
    r'^\s*git clone',
    r'^\s*armbian-install',
    r'^\s*ls -la',
    r'^\s*lsmod',
    r'^\s*dmesg',
    r'^\s*cp ~/',
    r'^\s*chmod \+x',
    r'^\s*mkdir -p',
    r'^\s*wget ',
    r'^\s*scp -r',
    r'^\s*unzip ',
    r'^\s*cmake ',
    r'^\s*make -j',
    r'^\s*\./compile\.sh',
    r'^\s*sed -i',
    r'^\s*echo ',
    r'^\s*pip install',
    r'^\s*python3 -m venv',
    r'^\s*source ~/',
    r'^\s*picocom ',
    r'^\s*i2cdetect ',
    r'^\s*journalctl ',
    r'^\s*systemctl ',
    r'^\s*apt install',
]

# Команды, которые заканчивают bash-блок (пустая строка или заголовок)
def is_end_of_code(line):
    stripped = line.strip()
    if not stripped:
        return True
    if stripped.startswith('#'):
        return False  # комментарий внутри кода
    if stripped.startswith('**'):
        return True
    if stripped.startswith('|'):
        return True
    if stripped.startswith('- ') or stripped.startswith('* '):
        return True
    if stripped.startswith('###') or stripped.startswith('##'):
        return True
    if stripped.startswith('```'):
        return True
    return False

i = 0
while i < len(lines):
    line = lines[i]
    stripped = line.strip()
    
    # Если мы внутри код-блока
    if in_code_block:
        # Проверяем, не начался ли новый код-блок
        is_new_code = False
        for pattern, lang in code_start_patterns:
            if re.match(pattern, stripped):
                # Закрываем текущий блок
                output.append('```\n')
                output.append('\n')
                in_code_block = False
                is_new_code = True
                break
        
        if is_new_code:
            output.append(line)
            in_code_block = True
            continue
        
        # Проверяем, не закончился ли блок
        if is_end_of_code(line):
            output.append('```\n')
            output.append('\n')
            in_code_block = False
            output.append(line)
            continue
        
        output.append(line)
        i += 1
        continue
    
    # Если мы НЕ внутри код-блока
    # Проверяем, не начинается ли код-блок
    is_code_start = False
    for pattern, lang in code_start_patterns:
        if re.match(pattern, stripped):
            is_code_start = True
            break
    
    if is_code_start:
        output.append(line)
        in_code_block = True
        i += 1
        continue
    
    # Проверяем bash-команды без явного ```bash
    for pattern in bash_start_patterns:
        if re.match(pattern, stripped):
            output.append('```bash\n')
            output.append(line)
            in_code_block = True
            break
    
    if in_code_block:
        i += 1
        continue
    
    output.append(line)
    i += 1

# Закрываем последний блок, если он открыт
if in_code_block:
    output.append('```\n')

# Записываем результат
with open('PROJECT.md', 'w') as f:
    f.writelines(output)

print("PROJECT.md исправлен!")
print(f"Строк: {len(output)}")
