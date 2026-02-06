# Backend Error Notification System

## Overview
Added comprehensive error notification system to index.html to inform users about backend connection issues (persistent or occasional).

## Features

### 1. **Persistent Error Banner**
- **Location**: Fixed at the top of the page (below navigation bar)
- **Appearance**: Red gradient banner with error details
- **Trigger**: Shows after 3 consecutive backend failures
- **Content**:
  - Main error message
  - Detailed stats (consecutive failures, total failures, context)
  - "Retry Now" button for manual retry

### 2. **Error Tracking**
The system tracks:
- **Consecutive failures**: Count of back-to-back errors
- **Total failures**: Lifetime count since page load
- **Last error**: Timestamp, context, and error details
- **Error context**: Which operation failed (status polling, app initialization, etc.)

### 3. **Smart Notifications**

#### Occasional Errors (1-2 failures)
- Logged to console only
- No user notification
- Banner does NOT show

#### Persistent Errors (3+ consecutive failures)
- Error banner appears at top
- Shows "Backend connection issues detected"
- Details include failure counts and context

#### Severe Errors (10+ consecutive failures)
- Banner message changes to "Backend appears to be down"
- Suggests more serious issue

### 4. **Auto-Recovery**
When backend recovers:
- Success toast notification appears
- Error banner automatically dismisses
- Consecutive failure counter resets

### 5. **Manual Retry**
- "Retry Now" button in error banner
- Resets error counters
- Shows "Retrying connection..." toast
- Automatic polling resumes

## Error Contexts Tracked

1. **device info** - Camera device information fetch
2. **app initialization** - Application settings and configuration
3. **status polling** - Real-time status updates (every 300ms)

## User Experience

### Scenario 1: Brief Connection Hiccup
```
Request 1: ✓ Success
Request 2: ✗ Fail (logged to console)
Request 3: ✗ Fail (logged to console)
Request 4: ✓ Success → No banner shown, auto-recovered
```

### Scenario 2: Persistent Issue
```
Request 1-2: ✓ Success
Request 3-5: ✗ Fail (3 consecutive)
→ RED BANNER APPEARS: "Backend connection issues detected"
→ Shows: "Failures: 3 consecutive, 3 total | Context: status polling"

Request 6-15: ✗ Fail (13 consecutive)
→ Banner updates: "Backend appears to be down"
→ Shows: "Failures: 13 consecutive, 13 total | Context: status polling"

Request 16: ✓ Success
→ GREEN TOAST: "Backend connection restored"
→ Banner disappears
```

## Technical Details

### Configuration
```javascript
backendErrors.threshold: 3  // Show banner after this many consecutive failures
```

### Error State Object
```javascript
backendErrors = {
    consecutiveFailures: 0,     // Current streak
    lastError: null,            // Last error details
    totalFailures: 0,           // Total count
    startTime: Date.now(),      // Page load time
    showingBanner: false,       // Banner visibility
    threshold: 3                // Trigger threshold
}
```

## Styling

- **Colors**: Red gradient (#dc2626 to #b91c1c)
- **Animation**: Smooth slide-down on appearance
- **Position**: Fixed at top (z-index: 9999)
- **Mobile**: Responsive design

## Integration Points

All AJAX error handlers now call:
```javascript
handleBackendError(context, error)
```

All AJAX success handlers now call:
```javascript
handleBackendSuccess()
```

## Benefits

1. **Users are informed**: No silent failures
2. **Context-aware**: Shows what operation failed
3. **Non-intrusive**: Only alerts after multiple failures
4. **Auto-recovery**: Dismisses automatically when fixed
5. **Manual control**: Retry button for immediate action
6. **Detailed info**: Helps with troubleshooting

## Future Enhancements

Possible additions:
- Different thresholds per context
- Error history log viewer
- Automatic page reload after prolonged failure
- Backend health check endpoint
- Email/alert notifications for admins
