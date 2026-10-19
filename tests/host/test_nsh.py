#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the actual nsh loop, buffered script reader and parser on host files."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

SIM = Path(__file__).resolve().parents[2] / 'build/host/sim_nsh'


class ShellTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nsh-')
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def file(self, name, text):
        path = self.root / name
        path.write_bytes(text if isinstance(text, bytes) else text.encode())
        return path

    def run_shell(self, commands, *options, allow_stderr=False):
        p = subprocess.run([SIM, *options], input=commands + '\nexit\n', text=True,
                           capture_output=True, timeout=5)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        if not allow_stderr:
            self.assertFalse(p.stderr, p.stderr)
        return p.stdout + p.stderr

    def script(self, text, *options, allow_stderr=False):
        path = self.file('main', text)
        out = self.run_shell(f'sh {path}\necho result=$?', *options, allow_stderr=allow_stderr)
        return out, int(re.search(r'result=(\d+)', out)[1])

    def test_if_elif_else(self):
        out, status = self.script('if false; then\necho BAD\nelif true; then\necho selected\n'
                                  'elif true\nthen\necho BAD\nelse\necho BAD\nfi\n')
        self.assertIn('selected\n', out)
        self.assertNotIn('BAD', out)
        self.assertEqual(status, 0)

    def test_separate_keywords_and_crlf(self):
        out, status = self.script(b'if false\r\nthen\r\necho BAD\r\nelse\r\necho selected\r\nfi')
        self.assertIn('selected\n', out)
        self.assertNotIn('BAD', out)
        self.assertEqual(status, 0)

    def test_loop_reexpands_and_crosses_read_buffer(self):
        out, status = self.script('#' + ' ' * 120 + '\nset I 0\nwhile [ "$I" -lt 12 ]; do\n'
                                  '# a body longer than the read-ahead buffer, revisited each time\n'
                                  'echo item-$I\ninc I\ndone\necho final-$I\n')
        self.assertEqual(re.findall(r'item-(\d+)', out), [str(i) for i in range(12)])
        self.assertIn('final-12', out)
        self.assertEqual(status, 0)

    def test_break_continue_nested_in_if(self):
        out, status = self.script('set I 0\nwhile true\ndo\ninc I\nif [ $I -eq 2 ]; then\n'
                                  'continue\nfi\nif [ $I -eq 4 ]; then\nbreak\nfi\necho item-$I\ndone\necho final-$I\n')
        self.assertEqual(re.findall(r'item-(\d+)', out), ['1', '3'])
        self.assertIn('final-4', out)
        self.assertEqual(status, 0)

    def test_nested_loops_break_only_inner(self):
        out, status = self.script('set I 0\nwhile [ $I -lt 3 ]; do\ninc I\nwhile true; do\n'
                                  'echo outer-$I\nbreak\ndone\ndone\n')
        self.assertEqual(re.findall(r'outer-(\d+)', out), ['1', '2', '3'])
        self.assertEqual(status, 0)

    def test_skipped_branch_has_no_side_effects_or_expansion(self):
        target = self.root / 'bad'
        out, status = self.script(f'set BIG {"x" * 105}\nif false; then\necho $BIG$BIG\n'
                                  f'echo BAD > {target}\nsh /no-such-file\n'
                                  'while true; do\necho BAD\ndone\nelse\necho selected\nfi\n')
        self.assertFalse(target.exists())
        self.assertNotIn('BAD', out)
        self.assertNotIn('expanded line too long', out)
        self.assertEqual(status, 0)

    def test_quoted_semicolons_and_comments(self):
        out, status = self.script("if test 'a;b' = \"a;b\"; then # ; ignored\necho selected\nfi\n"
                                  'if true # ; then is a comment\nthen\necho also-selected\nfi\n')
        self.assertIn('selected\n', out)
        self.assertIn('also-selected\n', out)
        self.assertEqual(status, 0)

    def test_false_loop_never_runs(self):
        out, status = self.script('while false; do\necho BAD\ndone\n')
        self.assertNotIn('BAD', out)
        self.assertEqual(status, 0)

    def test_compound_status(self):
        for text, expected in [('if false; then\ntrue\nfi\n', 0),
                               ('if true; then\nfalse\nfi\n', 1),
                               ('if false; then\ntrue\nelse\nfalse\nfi\n', 1),
                               ('set I 0\nwhile [ $I -eq 0 ]; do\ninc I\nfalse\ndone\n', 1)]:
            with self.subTest(text=text):
                self.assertEqual(self.script(text)[1], expected)

    def test_sourced_script_in_loop_resumes_buffer_and_scope(self):
        child = self.file('child', 'if true; then\ninc I\nfi\nexit\necho BAD\n')
        out, status = self.script(f'set I 0\nwhile [ $I -lt 4 ]; do\nsh {child}\necho item-$I\ndone\n')
        self.assertEqual(re.findall(r'item-(\d+)', out), ['1', '2', '3', '4'])
        self.assertNotIn('BAD', out)
        self.assertEqual(status, 0)

    def test_child_cannot_close_callers_block(self):
        child = self.file('child', 'fi\necho BAD\n')
        out, status = self.script(f'if true; then\nsh {child}\necho resumed\nfi\n')
        self.assertIn('unexpected control word', out)
        self.assertIn('resumed', out)
        self.assertNotIn('BAD', out)
        self.assertEqual(status, 0)

    def test_child_cannot_break_callers_loop(self):
        child = self.file('child', 'break\necho BAD\n')
        out, _ = self.script(f'while true; do\nsh {child}\nbreak\ndone\n')
        self.assertIn('outside a loop', out)
        self.assertNotIn('BAD', out)

    def test_child_status_is_returned_to_caller(self):
        child = self.file('child', 'if true; then\nfalse\nfi\n')
        self.assertEqual(self.script(f'if true; then\nsh {child}\nfi\n')[1], 1)

    def test_ctrl_c_unwinds_all_scripts(self):
        child = self.file('child', 'while true; do\ntrue\ndone\n')
        out, status = self.script(f'sh {child}\necho BAD\n', '20')
        self.assertEqual(status, 130)
        self.assertNotIn('BAD', out)

    def test_ctrl_c_from_command_stops_script(self):
        out, status = self.script('while true; do\nsleep 10\ndone\necho BAD\n', '50')
        self.assertEqual(status, 130)
        self.assertNotIn('BAD', out)

    def test_buffer_allocation_failure_uses_byte_reads(self):
        out, status = self.script('set I 0\nwhile [ $I -lt 2 ]; do\ninc I\ndone\necho final-$I\n', '--no-buffer')
        self.assertIn('final-2', out)
        self.assertEqual(status, 0)

    def test_seek_failure_stops_loop(self):
        out, status = self.script('while true; do\necho once\ndone\necho BAD\n', '--fail-seek')
        self.assertEqual(status, 2)
        self.assertIn('cannot seek', out)
        self.assertEqual(out.count('once'), 1)
        self.assertNotIn('BAD', out)

    def test_failed_child_rewind_preserves_parent_buffer(self):
        child = self.file('c', 'echo CHILD\n')
        for command in ('sh', '.'):
            with self.subTest(command=command):
                out, status = self.script(f'{command} {child}\necho child-status=$?\necho AFTER\n',
                                          '--fail-rewind')
                self.assertIn('cannot rewind parent script', out)
                self.assertIn('child-status=1\n', out)
                self.assertIn('AFTER\n', out)
                self.assertNotIn('CHILD', out)
                self.assertEqual(status, 0)

    def test_failed_child_rewind_preserves_loop_and_can_retry(self):
        child = self.file('c', 'echo CHILD\n')
        out, status = self.script(f'set I 0\nwhile [ $I -lt 2 ]; do\nsh {child}\n'
                                  'echo child-status=$?\ninc I\ndone\necho AFTER-$I\n', '--fail-rewind')
        self.assertEqual(re.findall(r'child-status=(\d+)', out), ['1', '0'])
        self.assertEqual(out.count('CHILD\n'), 1)
        self.assertIn('AFTER-2\n', out)
        self.assertEqual(status, 0)

    def test_child_without_read_ahead_needs_no_rewind(self):
        child = self.file('c', 'echo CHILD\n')
        out, status = self.script(f'sh {child}\necho AFTER\n', '--no-buffer', '--fail-rewind')
        self.assertIn('CHILD\n', out)
        self.assertIn('AFTER\n', out)
        self.assertNotIn('cannot rewind', out)
        self.assertEqual(status, 0)

    def test_redirection_close_failure_is_reported(self):
        target = self.root / 'output'
        out, status = self.script(f"printf '%s' data > {target}\necho saved=$?\n", '--fail-close', allow_stderr=True)
        self.assertEqual(target.read_bytes(), b'data')
        self.assertIn('saved=1\n', out)
        self.assertIn('redirection close failed', out)
        self.assertEqual(status, 0)  # following echo ran on the restored terminal

    def test_redirection_close_failure_preserves_command_error(self):
        target = self.root / 'output'
        out, status = self.script(f"printf '%B' 256 > {target}\n", '--fail-close', allow_stderr=True)
        self.assertEqual(status, 2)
        self.assertEqual(target.read_bytes(), b'')
        self.assertIn('redirection close failed', out)

    def test_input_redirection_restores_terminal_after_open_failure(self):
        source = self.file('input', 'data')
        out, status = self.script(f'cat < {source} > /no-such-parent/output\necho restored\n')
        self.assertIn('restored\n', out)
        self.assertEqual(status, 0)

    def test_job_commands_honor_redirection(self):
        for command, expected in [('jobs', b''), ('wait', b''), ('kill', b'usage: kill %JOB|PID ...\n')]:
            with self.subTest(command=command):
                target = self.root / command
                out, _ = self.script(f'{command} > {target}\necho restored\n')
                self.assertEqual(target.read_bytes(), expected)
                self.assertNotIn('usage: kill', out)
                self.assertIn('restored\n', out)

    def test_cancellation_status_survives_redirection_close_failure(self):
        target = self.root / 'output'
        main = self.file('main', f'cat /dev/tty > {target}\necho BAD\n')
        out = self.run_shell(f'sh {main}\n\x03\necho status=$?', '--fail-close', allow_stderr=True)
        self.assertIn('status=130\n', out)
        self.assertIn('redirection close failed', out)
        self.assertNotIn('BAD', out)

    def test_bad_structure_aborts_current_script(self):
        cases = ['then\n', 'else\n', 'fi\n', 'do\n', 'done\n', 'break\n', 'continue\n',
                 'if\n', 'while\n', 'if true\necho BAD\n', 'while true\necho BAD\n',
                 'if true; then\nelse\nelse\n', 'if true; then\nelse\nelif true; then\n',
                 'if true; then\ndone\n', 'while true; do\nfi\n',
                 'if true; then echo BAD\n', 'while true; do echo BAD\n',
                 'if true; then\nfi extra\n', 'while true; do\nbreak 2\n']
        for text in cases:
            with self.subTest(text=text):
                out, status = self.script(text + 'echo BAD\n')
                self.assertEqual(status, 2, out)
                self.assertNotIn('BAD\n', out)

    def test_unterminated_block_at_eof(self):
        for text in ['if true; then\ntrue\n', 'while false; do\ntrue\n']:
            out, status = self.script(text)
            self.assertEqual(status, 2)
            self.assertIn('unterminated', out)

    def test_maximum_nesting(self):
        out, status = self.script('if true; then\n' * 8 + 'echo selected\n' + 'fi\n' * 8)
        self.assertEqual(status, 0)
        self.assertIn('selected', out)
        out, status = self.script('if true; then\n' * 9 + 'echo BAD\n' + 'fi\n' * 9)
        self.assertEqual(status, 2)
        self.assertIn('nesting too deep', out)
        self.assertNotIn('BAD', out)

    def test_long_binary_lines_rejected_before_redirection(self):
        target = self.root / 'bad'
        for suffix in [b' ' + b'x' * 160, b'\0BAD']:
            out, status = self.script(f'echo BAD > {target}'.encode() + suffix + b'\n')
            self.assertEqual(status, 2)
            self.assertIn('script read failed', out)
            self.assertFalse(target.exists())

    def test_condition_cannot_source_or_exit(self):
        for condition in ['sh /no-such-file', 'exit', 'logout']:
            out, status = self.script(f'if {condition}; then\necho BAD\nfi\n')
            self.assertEqual(status, 2)
            self.assertIn('cannot be a condition', out)
            self.assertNotIn('BAD', out)

    def test_interactive_control_flow_explains_script_requirement(self):
        out = self.run_shell('if true\necho result=$?')
        self.assertIn('requires sh FILE', out)
        self.assertIn('result=2', out)

    def test_boot_script_uses_same_control_flow(self):
        path = self.file('rc', 'set I 0\nwhile [ $I -lt 2 ]; do\ninc I\ndone\n'
                         'if [ $I -eq 2 ]; then\necho boot-ok\nfi\n')
        out = self.run_shell('', '--boot', str(path))
        self.assertEqual(out, 'boot-ok\n')

    def test_true_false_and_test_status(self):
        values = {'true': 0, 'false': 1, 'test': 1, '[ ]': 1, 'test ""': 1,
                  'test hello': 0, 'test -n hello': 0, 'test -z ""': 0,
                  'test ! hello': 1, '[ ! -n "" ]': 0,
                  '[ x = x ]': 0, '[ x != x ]': 1, '[ x != y ]': 0,
                  '[ -5 -lt 0 ]': 0, '[ +5 -ge 5 ]': 0, '[ 5 -ne 6 ]': 0,
                  '[ -2147483648 -le 2147483647 ]': 0,
                  '[ 2147483648 -gt 0 ]': 2, '[ -2147483649 -lt 0 ]': 2,
                  '[ x -eq 0 ]': 2, '[ x = x': 2, 'test x extra': 2}
        for command, expected in values.items():
            with self.subTest(command=command):
                out = self.run_shell(command + '\necho result=$?')
                self.assertIn(f'result={expected}\n', out)

    def test_file_predicates(self):
        full, empty = self.file('full', 'data'), self.file('empty', '')
        for condition, expected in [(f'-f {full}', 0), (f'-s {full}', 0), (f'-s {empty}', 1),
                                    (f'-e {empty}', 0), (f'-d {self.root}', 0), (f'-f {self.root}', 1),
                                    (f'-e {self.root}/missing', 1)]:
            out = self.run_shell(f'[ {condition} ]\necho result=$?')
            self.assertIn(f'result={expected}\n', out)

    def test_increment_decrement_and_overflow(self):
        out = self.run_shell('inc I\necho value=$I\ninc I -3\necho value=$I\n'
                             'set I 2147483647\ninc I\necho result=$?\necho value=$I\n'
                             'set I -2147483648\ninc I -1\necho result=$?\necho value=$I')
        self.assertIn('value=1\n', out)
        self.assertIn('value=-2\n', out)
        self.assertIn('value=2147483647\n', out)
        self.assertIn('value=-2147483648\n', out)
        self.assertEqual(out.count('result=2\n'), 2)

    def test_variables_and_status_from_existing_shell(self):
        out = self.run_shell('set X "two words"\necho "$X"\nfalse\necho code=$?\nunset X\necho empty="$X"')
        self.assertIn('two words\n', out)
        self.assertIn('code=1\n', out)
        self.assertIn('empty=\n', out)

    def test_script_positional_arguments(self):
        path = self.file('args', 'echo "zero=$0" "count=$#"\necho "one=$1" "two=${2}" "missing=$9"\n')
        out = self.run_shell(f'sh {path} "hello world" second\necho outside=$#')
        self.assertIn(f'zero={path} count=2\n', out)
        self.assertIn('one=hello world two=second missing=\n', out)
        self.assertIn('outside=0\n', out)

    def test_all_arguments_preserve_empty_words_and_operators(self):
        child = self.file('child', 'echo "count=$#"\necho "first=<$1>"\necho "empty=<$2>"\necho "last=<$3>"\n')
        parent = self.file('parent', f'sh {child} "$@"\n')
        literal = '| > & ; # " \\'
        out = self.run_shell(f"sh {parent} 'two words' '' '{literal}'")
        self.assertIn('count=3\n', out)
        self.assertIn('first=<two words>\n', out)
        self.assertIn('empty=<>\n', out)
        self.assertIn(f'last=<{literal}>\n', out)

    def test_all_arguments_with_empty_list(self):
        child = self.file('child', 'echo count=$#\n')
        parent = self.file('parent', f'sh {child} "$@"\n')
        out = self.run_shell(f'sh {parent}')
        self.assertIn('count=0\n', out)

    def test_shift_and_nested_script_restore(self):
        child = self.file('child', 'echo child-before=$#:$1\nshift\necho child-after=$#:$1\n')
        parent = self.file('parent', f'echo parent-before=$#:$1\nsource {child} inner one\n'
                           'echo parent-after=$#:$1\nshift 2\necho parent-shifted=$#:$1\n')
        out = self.run_shell(f'sh {parent} outer two three')
        self.assertIn('parent-before=3:outer\n', out)
        self.assertIn('child-before=2:inner\n', out)
        self.assertIn('child-after=1:one\n', out)
        self.assertIn('parent-after=3:outer\n', out)
        self.assertIn('parent-shifted=1:three\n', out)

    def test_loop_over_positional_arguments(self):
        path = self.file('loop', 'while [ "$#" -gt 0 ]; do\necho "arg=<$1>"\nshift\ndone\n')
        out = self.run_shell(f'sh {path} first "two words" ""')
        self.assertEqual(re.findall(r'arg=<([^\n]*)>', out), ['first', 'two words', ''])

    def test_shift_bounds_do_not_change_arguments(self):
        path = self.file('shift', 'shift 0\necho zero=$#:$1\nshift 3\necho code=$? count=$# first=$1\n'
                         'shift -1\necho code=$? count=$#\nshift 2\necho empty=$#:$1\n'
                         'shift\necho code=$? count=$#\n')
        out = self.run_shell(f'sh {path} first second')
        self.assertIn('zero=2:first\n', out)
        self.assertIn('code=2 count=2 first=first\n', out)
        self.assertIn('code=2 count=2\n', out)
        self.assertIn('empty=0:\n', out)
        self.assertIn('code=2 count=0\n', out)

    def test_maximum_nine_script_arguments(self):
        child = self.file('child', 'echo count=$# ninth=$9\n')
        parent = self.file('parent', f'sh {child} "$@"\n')
        out = self.run_shell(f'sh {parent} 1 2 3 4 5 6 7 8 9')
        self.assertIn('count=9 ninth=9\n', out)

    def test_no_positional_argument_inheritance_without_explicit_args(self):
        child = self.file('child', 'echo child=$#:$1\n')
        parent = self.file('parent', f'. {child}\necho parent=$#:$1\n')
        out = self.run_shell(f'sh {parent} outer')
        self.assertIn('child=0:\n', out)
        self.assertIn('parent=1:outer\n', out)

    def test_printf_writes_binary_and_appends_without_newline(self):
        path = self.root / 'bytes'
        out = self.run_shell(f"printf '\\x00\\xff' > {path}\nprintf '%B' 128 0 65 >> {path}\necho code=$?")
        self.assertIn('code=0', out)
        self.assertEqual(path.read_bytes(), bytes([0, 255, 128, 0, 65]))

    def test_printf_uses_script_arguments_and_reuses_format(self):
        target = self.root / 'args'
        path = self.file('args.sh', f'printf "<%s>" "$@" > {target}\n')
        self.run_shell(f'sh {path} "two words" "" third')
        self.assertEqual(target.read_bytes(), b'<two words><><third>')

    def test_printf_data_does_not_become_a_format(self):
        target = self.root / 'data'
        self.run_shell(f"printf '%s' '%B\\x00' > {target}")
        self.assertEqual(target.read_bytes(), b'%B\\x00')

    def test_printf_generates_all_256_bytes_in_a_loop(self):
        target = self.root / 'all'
        path = self.file('all.sh', f"printf '' > {target}\nset I 0\nwhile [ $I -lt 256 ]; do\n"
                         f"printf '%B' \"$I\" >> {target}\ninc I\ndone\nunset I\n")
        self.run_shell(f'sh {path}')
        self.assertEqual(target.read_bytes(), bytes(range(256)))

    def test_lcd_command_removed(self):
        out = self.run_shell('lcd text 0 0 test\necho code=$?')
        self.assertIn('lcd: command not found', out)
        self.assertIn('code=127', out)

    def test_cat_interrupt_stops_scripts_and_does_not_wait_on_next_file(self):
        data = self.file('data', b'a' * 512)
        path = self.file('cat.sh', f'cat {data} /dev/tty > /dev/null\necho BAD\n')
        # A terminal Ctrl-C while cat waits is handled as status 130.
        out = self.run_shell(f'sh {path}\n\x03\necho code=$?')
        self.assertNotIn('BAD', out)
        self.assertIn('code=130', out)

    def test_raw_box_script_generates_its_own_frame(self):
        image = self.root / 'box.raw'
        path = SIM.parents[2] / 'examples/scripts/raw-box.sh'
        out = self.run_shell(f'sh {path} {image}\n\x04\necho code=$?')
        self.assertIn('code=0', out)
        frame = image.read_bytes()
        self.assertEqual(len(frame), 1024)
        expected = bytes(255 if 2 <= page <= 5 and 24 <= x <= 103 else 0
                         for page in range(8) for x in range(128))
        self.assertEqual(frame, expected)

    def test_cat_interrupt_at_prompt_keeps_shell_alive(self):
        out = self.run_shell('cat > /dev/null\n\x03\necho code=$?\necho alive')
        self.assertIn('code=130', out)
        self.assertIn('alive', out)


if __name__ == '__main__':
    unittest.main()
