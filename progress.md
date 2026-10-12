Day #1
Set up WSL and Linux Setup and created test programs for the debugger

Day #2
Implemented Stack for Stage 1 validation of test programs and source.bin. Implemented Key Functions such as readsourceline, readfirstword, readsecondword, and subsequent helper functions which were needed for main validate Function. Will start from stage 2 tomorrow which will be the resolve part

Day#3
Completed Stage by Implementing WriteResolveRecord, ReadResolveRecord, Resolve Program. Calculates starting point of each line, calculates offset by adding start + previous offset and size and stores text that many bytes later and returns the where main starts. Resolve.bin stores all lines and starting positions and call statements are patched/stored to be resolved after the main has been passed through, they return the offset of the original function call

Day#4 Completed Stage 3 which executes the entire program by using doubly linked list(Timeline) which stores a snapshot of each executed line and each node holds the address of its previous and next node so we can see both ways. with the help of a few helper functions used to identify local variables and arithmetic operations during real time execution and tokenization functions that we used earlier to see that function header is valid eg a value cannot be assigned to a number only a variable, resolve.bin returns main address and we read the main address and call stack frame to push main. Using Patching from Stage 2 to handle offset values in call lines.
