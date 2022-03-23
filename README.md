## Architecture: 3-stage Pipeline

We build a 3-stage pipeline to streamline the processing of command-lines:

1. **Command-line Tokenization**: transform a stream of _characters_ into a list
   of _tokens_
2. **Command-line Parsing**: transform _tokens_ into _processes_
3. **Process Execution**: execute _processes_ and collect exit statuses

### Data Structures

- **Token**: a _token_ is a string that is either a _command-line argument_
  (such as `ls`, `-a`) or _an operator_ (`>`, `|`, `>&`, or `|&`). A token
  serves as an intermediary data exchange format between stage 1 and stage 2.

- **Process**: a _process_ is a data structure that contains all the information
  necessary to execute one command _independently_. It contains an argument list
  (`argv`) and file descriptors for input (`fd_in`), output (`fd_out`), and
  error (`fd_err`). As an intermediary data exchange format between stage 2 and
  stage 3, such a design help separate the logic for setting up pipes and output
  redirection with the logic for executing the process, so each _process_ is
  independent from process executor's perspective because pipes and output
  redirections are already encoded in the file descriptor fields implicitly
  during parsing in stage 2.

### Stage 1: Command-line Tokenization (Lexical Analysis)

Command-line tokenization is the first stage of the pipeline. In this stage, the
command-line input string is split into _tokens_. It not only splits the string
based on whitespaces but also separates an operator from its operands even when
there is no whitespace among them. For example, `xx yy>zz` will become `xx`,
`yy`, `>`, `zz`.

### Stage 2: Command-line Parsing

After tokenization, the next stage reads the tokens and constructs _processes_.
This stage mainly has the following 3 responsibilities:

- constructing the argument list for each command (`argv`),
- setting up the pipes between processes (if necessary), and
- setting up the output redirection and/or error redirection (if necessary).

Note that there are no `dup2` calls here. We're only setting the file
descriptors in the _process_ struct.

**Setting up the pipes**

To set up the pipes between two processes, we first call `pipe` to create 2 file
descriptors. Then we set the `fd_in` field of the destination _process_ struct
and the `fd_out` & `fd_err` fields of the source _process_ struct.

**Setting up output and/or error redirection**

To set up output redirection for a process, we first call `open` to create a
file descriptor. Then we set the `fd_out` & `fd_err` fields of the _process_
struct.

### Stage 3: Process Execution

After parsing the command, we determine if the first command is a built-in
command or external command. If it is a built-in command, we call the
corresponding function directly without entering `run_procs` .

For external commands, we call `run_procs` that will launch each process in a
loop. Within the loop, we call `fork` to generate the child to execute the
process using `exec_proc`. Both parent and child will close file-descriptors
they don't need by calling`close_nonstd_fds`. Specifically, the parent will
close the file descriptors used by the child, while the child will close file
descriptors used by other children.

At this point, each child is independent, since file descriptors needed for each
process are opened and contained in its own _process_ struct. Each child will
manipulate its file descriptors by calling `dup2` according to the values (
`fd_in`, `fd_out`, and `fd_err` fields) set in its _process_ struct.

Note that each child does not need to know whether a specific file descriptor is
connected to a pipe or a redirected output file. It simply duplicates the file
descriptors according to what is specified in the _process_ struct.

After `dup2` call, each child closes the duplicated file descriptors and call
`execvp` with `argv` filed in its _process_ struct to become another process.

The parent, after launching all the processes, will wait for all children to
exit by calling `waitpid` in a loop and collect the exit status codes.

## Testing

To make sure that our design closely follows the specifications, we performed 3
types of testing during different stages of development.

### Testing during Debugging

During debugging, we printed output and error to the terminal. When debugging
output redirection and/or error redirection, we opened the file we redirected to
and check the content of the file manually.

### Testing using Provided Testing Script

After we finished debugging, we tested our code using the testing script on the
CSIF. We looked into the testing script to help us pinpoint the bugs in our code
when a test case fails.

### Testing using Automated Script against Reference Program

Before we submit our project, we wrote a Python script that compare the output
of our implementation to the output from the reference program provided. Our the
script executes the same command lines we passed in both sshell implementations
redirects the standard output and error of both implementations to different
files, and use `diff` to display the differences between the redirected files.
Out test cases include not only those available on the project specs but also
extra ones that cover more edge cases based on the feature specification.

### Enviorment
This project is built for Linux system.

