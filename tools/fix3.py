#!/usr/bin/env python3
"""
Финальное исправление PROJECT.md
"""
import re

with open('PROJECT.md', 'r') as f:
    content = f.read()

# ============================================================
# 1. Убираем дублирующиеся разделители таблиц
# ============================================================
# Заменяем "|---|---|---|\n|---|---|---|" на "|---|---|---|"
content = re.sub(r'(\|---+\|)+\n(\|---+\|)+\n', r'\1\n', content)

# ============================================================
# 2. Исправляем "bash" как текст (без ```)
# ============================================================
# Заменяем строку, состоящую только из "bash", на "```bash"
content = re.sub(r'^bash\s*$', '```bash', content, flags=re.MULTILINE)

# ============================================================
# 3. Закрываем незакрытые код-блоки
# ============================================================
lines = content.split('\n')
output = []
in_code = False

for i, line in enumerate(lines):
    stripped = line.strip()
    
    # Начало код-блока
    if stripped.startswith('```') and not stripped == '```':
        if in_code:
            output.append('```')
            output.append('')
        output.append(line)
        in_code = True
        continue
    
    # Конец код-блока
    if stripped == '```':
        output.append(line)
        in_code = False
        continue
    
    # Если мы в код-блоке, проверяем, не закончился ли он
    if in_code:
        # Признаки конца: пустая строка + не команда, заголовок, таблица
        if stripped and not any([
            stripped.startswith('#'),
            stripped.startswith('sudo'),
            stripped.startswith('cd '),
            stripped.startswith('git '),
            stripped.startswith('cp '),
            stripped.startswith('chmod'),
            stripped.startswith('mkdir'),
            stripped.startswith('wget'),
            stripped.startswith('scp '),
            stripped.startswith('unzip'),
            stripped.startswith('cmake'),
            stripped.startswith('make '),
            stripped.startswith('./'),
            stripped.startswith('sed '),
            stripped.startswith('echo '),
            stripped.startswith('pip '),
            stripped.startswith('python3'),
            stripped.startswith('source'),
            stripped.startswith('picocom'),
            stripped.startswith('i2cdetect'),
            stripped.startswith('journalctl'),
            stripped.startswith('systemctl'),
            stripped.startswith('apt '),
            stripped.startswith('ls '),
            stripped.startswith('lsmod'),
            stripped.startswith('dmesg'),
            stripped.startswith('armbian-install'),
            stripped.startswith('overlays='),
        ]):
            # Это не команда — закрываем блок
            output.append('```')
            output.append('')
            in_code = False
    
    output.append(line)

# Закрываем последний блок
if in_code:
    output.append('```')

content = '\n'.join(output)

# ============================================================
# 4. Убираем лишние пустые строки
# ============================================================
content = re.sub(r'\n{3,}', '\n\n', content)

# ============================================================
# 5. Записываем
# ============================================================
with open('PROJECT.md', 'w') as f:
    f.write(content)

print("PROJECT.md исправлен!")
print(f"Строк: {content.count(chr(10)) + 1}")
