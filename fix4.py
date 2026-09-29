#!/usr/bin/env python3
"""
Финальное исправление PROJECT.md
"""
import re

with open('PROJECT.md', 'r') as f:
    lines = f.readlines()

output = []
i = 0

# Языки, которые могут быть маркерами код-блока
LANGS = ['bash', 'python', 'javascript', 'html', 'css', 'ini', 'yaml', 'text', 'json', 'xml']

while i < len(lines):
    line = lines[i]
    stripped = line.strip()
    
    # 1. Пропускаем пустые строки между строками таблицы
    if stripped == '' and output and output[-1].strip().startswith('|'):
        # Проверяем, следующая строка - тоже таблица
        if i + 1 < len(lines) and lines[i+1].strip().startswith('|'):
            i += 1
            continue
    
    # 2. Заменяем "bash" / "python" / и т.д. (как текст) на ```bash
    if stripped in LANGS:
        output.append(f'```{stripped}\n')
        i += 1
        # Собираем содержимое код-блока
        while i < len(lines):
            l = lines[i]
            s = l.strip()
            if s == '```' or s in LANGS:
                break
            if s == '' and i + 1 < len(lines) and lines[i+1].strip() not in LANGS and not lines[i+1].strip().startswith(('|', '-', '#', '**', '*')):
                break
            output.append(l)
            i += 1
        output.append('```\n')
        output.append('\n')
        continue
    
    # 3. Убираем пустые ```bash / ``` блоки
    if stripped in ('```bash', '```python', '```javascript', '```html', '```css', '```ini', '```yaml', '```text'):
        # Проверяем, следующая строка - ```
        if i + 1 < len(lines) and lines[i+1].strip() == '```':
            i += 2
            continue
    
    # 4. Обрабатываем обычный текст
    output.append(line)
    i += 1

content = ''.join(output)

# 5. Убираем лишние пустые строки
content = re.sub(r'\n{3,}', '\n\n', content)

# 6. Исправляем таблицы JSON-команд (табы → |)
# Ищем таблицы вида "Команда\tJSON\tОписание"
lines = content.split('\n')
result = []
in_json_table = False
for i, line in enumerate(lines):
    if line.startswith('Команда\tJSON\tОписание') or line.startswith('Команда\tT\tОписание'):
        # Это заголовок таблицы
        parts = line.split('\t')
        result.append('| ' + ' | '.join(parts) + ' |')
        result.append('|' + '---|' * len(parts))
        in_json_table = True
        continue
    
    if in_json_table:
        if '\t' in line and not line.startswith('|'):
            parts = line.split('\t')
            result.append('| ' + ' | '.join(parts) + ' |')
            continue
        else:
            in_json_table = False
    
    result.append(line)

content = '\n'.join(result)

# 7. Записываем
with open('PROJECT.md', 'w') as f:
    f.write(content)

print("PROJECT.md исправлен!")
print(f"Строк: {content.count(chr(10)) + 1}")
