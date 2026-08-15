#!/bin/bash
# 脚本：list_text_files.sh
# 用途：递归列出当前目录下所有文本文件的名称和内容，输出到 output.txt

output_file="output.txt"
# 清空（或创建）输出文件
> "$output_file"

# 递归查找所有普通文件（-type f），使用 null 分隔符处理含空格/特殊字符的文件名
find . -type f -print0 | while IFS= read -r -d '' file; do
    # 获取文件的 MIME 类型
    mime=$(file -b --mime-type "$file" 2>/dev/null)
    # 如果 MIME 类型以 text/ 开头，则认为是文本文件
    if [[ "$mime" == text/* ]]; then
        echo "===== $file =====" >> "$output_file"
        cat "$file" >> "$output_file"
        echo -e "\n" >> "$output_file"   # 文件之间空行分隔
    fi
done

echo "完成！结果已保存到 $(pwd)/$output_file"
