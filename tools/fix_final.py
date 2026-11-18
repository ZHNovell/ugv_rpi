#!/usr/bin/env python3
"""
Полное исправление PROJECT.md
"""
import re

with open('PROJECT.md', 'r') as f:
    content = f.read()

# ============================================================
# 1. Исправляем таблицу распиновки (убираем пустые строки между |)
# ============================================================
lines = content.split('\n')
output = []
in_table = False
prev_empty = False

for line in lines:
    stripped = line.strip()
    
    # Таблица
    if stripped.startswith('|'):
        if prev_empty and output and output[-1].strip().startswith('|'):
            # Убираем пустую строку между строками таблицы
            output.pop()
        output.append(line)
        in_table = True
        prev_empty = False
        continue
    
    # Пустая строка
    if stripped == '':
        if in_table:
            prev_empty = True
        else:
            output.append(line)
        continue
    
    in_table = False
    prev_empty = False
    output.append(line)

content = '\n'.join(output)

# ============================================================
# 2. Убираем дублирующиеся разделители таблиц
# ============================================================
content = re.sub(r'(\|---+\|)+\n(\|---+\|)+\n', r'\1\n', content)

# ============================================================
# 3. Исправляем таблицы JSON-команд (табы → |)
# ============================================================
content = content.replace('Команда\tJSON\tОписание', '| Команда | JSON | Описание |')
content = content.replace('Команда\tT\tОписание', '| Команда | T | Описание |')

# ============================================================
# 4. Убираем пустые ```bash / ``` блоки
# ============================================================
content = re.sub(r'```bash\n```\n', '', content)
content = re.sub(r'```python\n```\n', '', content)
content = re.sub(r'```text\n```\n', '', content)

# ============================================================
# 5. Закрываем незакрытые код-блоки
# ============================================================
lines = content.split('\n')
output = []
in_code = False
code_lang = None

for i, line in enumerate(lines):
    stripped = line.strip()
    
    # Открывающий ```lang
    m = re.match(r'^```(\w+)$', stripped)
    if m:
        if in_code:
            output.append('```')
            output.append('')
        output.append(line)
        in_code = True
        code_lang = m.group(1)
        continue
    
    # Закрывающий ```
    if stripped == '```':
        output.append(line)
        in_code = False
        code_lang = None
        continue
    
    # Внутри код-блока: проверяем, не закончился ли он
    if in_code:
        if stripped and not any([
            stripped.startswith('#'),
            stripped.startswith('sudo'), stripped.startswith('cd '),
            stripped.startswith('git '), stripped.startswith('cp '),
            stripped.startswith('chmod'), stripped.startswith('mkdir'),
            stripped.startswith('wget'), stripped.startswith('scp '),
            stripped.startswith('unzip'), stripped.startswith('cmake'),
            stripped.startswith('make '), stripped.startswith('./'),
            stripped.startswith('sed '), stripped.startswith('echo '),
            stripped.startswith('pip '), stripped.startswith('python3'),
            stripped.startswith('source'), stripped.startswith('picocom'),
            stripped.startswith('i2cdetect'), stripped.startswith('journalctl'),
            stripped.startswith('systemctl'), stripped.startswith('apt '),
            stripped.startswith('ls '), stripped.startswith('lsmod'),
            stripped.startswith('dmesg'), stripped.startswith('armbian-install'),
            stripped.startswith('overlays='), stripped.startswith('import '),
            stripped.startswith('from '), stripped.startswith('def '),
            stripped.startswith('if '), stripped.startswith('else'),
            stripped.startswith('elif'), stripped.startswith('try:'),
            stripped.startswith('except'), stripped.startswith('return'),
            stripped.startswith('self.'), stripped.startswith('print('),
            stripped.startswith('class '), stripped.startswith('@'),
            stripped.startswith('"""'), stripped.startswith("'''"),
            stripped.startswith('//'), stripped.startswith('/*'),
            stripped.startswith('*'), stripped.startswith('-'),
            stripped.startswith('+'), stripped.startswith('|'),
            stripped.startswith('['), stripped.startswith('{'),
            stripped.startswith('}'), stripped.startswith(')'),
            stripped.startswith('('), stripped.startswith('"'),
            stripped.startswith("'"), stripped.startswith('D'),
            stripped.startswith('I'), stripped.startswith('F'),
            stripped.startswith('L'), stripped.startswith('R'),
        ]):
            output.append('```')
            output.append('')
            in_code = False
            code_lang = None
    
    output.append(line)

if in_code:
    output.append('```')

content = '\n'.join(output)

# ============================================================
# 6. Убираем лишние пустые строки
# ============================================================
content = re.sub(r'\n{3,}', '\n\n', content)

# ============================================================
# 7. Записываем
# ============================================================
with open('PROJECT.md', 'w') as f:
    f.write(content)

print("PROJECT.md исправлен!")
print(f"Строк: {content.count(chr(10)) + 1}")
