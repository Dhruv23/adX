"""Scripts that are documentation and tests at once (phase_7.md 4.7).

Each imports ``adx`` (the scripting module), works on the open project - the app's in
the console, or one made with ``adx.new()`` - does all its work in one ``undo_group``
(so one Undo takes it back), and ends by asserting the result it documents.
``tests/python/test_scripting.py`` runs every one of them; a script that stops working
fails CI.
"""
